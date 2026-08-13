# Contract: NDNSF-DI Placement V3

**Status**: Planned  
**Compatibility**: V2 remains explicit preassembled single-device mode

## Public Application Call

The application supplies intent, not deployment:

```text
request(
  model = ModelRef(human_name, immutable_model_digest, artifact_profile?),
  task = TaskRef(adapter_contract, accelerator_requirement),
  input = application_payload,
  options = inference_options,
  deadline = invocation_deadline,
  strategy = optional_external_strategy
) -> InferenceRequestHandle
```

It does not provide roles, devices, shard paths, rank counts, or a precomputed
deployment. One public request ID is created at invocation and retained across
planning attempts.

### Required default wiring

The implementation boundary is normative, not illustrative:

```text
app_sdk/application.py
  normal request, no explicit legacy profile
    -> app_sdk/client.py constructs V3 placement coordinator
    -> LayerReuseFirstStrategy (default)
    -> Requester canonical-artifact ensure only
    -> app_sdk/placement.py seals RoleAssemblySpec values
    -> final ProviderSelectionProjectionV3
    -> provider.py / NativeProviderHandler consume projection
    -> Provider-local fetch, assembly, JIT admission, load, execution
```

For V3, `app_sdk/placement.py::_prepare_artifacts()` MUST NOT materialize or
publish a selected role-specific split. That legacy behavior is dispatched only
when the application explicitly selects
`PREASSEMBLED_PARTITION_SINGLE_DEVICE`; it then uses `PreSplitFirstStrategy` and
V2 wire/cache identities. A V3 planning or preparation failure never selects V2.

## Provider Configuration

Configuration restricts runtime-visible resources:

```yaml
provider:
  accelerator:
    mode: auto            # auto | none | explicit_subset
    devices: []           # container-visible selectors; used only for explicit_subset
  cpu_execution: allowed  # allowed | forbidden
  sharing_profile: exclusive-only
```

Rules:

- `auto` uses all healthy devices visible inside the Provider runtime boundary.
- `none` advertises no accelerator even if devices are visible.
- `explicit_subset` requires every selector to resolve; unresolved selectors
  fail startup/configuration reload.
- configuration cannot expose an unallocated device or invent capacity/topology.
- scheduler/container isolation is authoritative; configuration is not a
  security boundary.

## Provider Offer V3

```text
ProviderOfferV3 {
  version
  providerName
  serviceName
  bootEpoch
  offerExpiry
  topologyProfileDigest
  resourceSnapshotDigest
  resourceSequence
  topologySummary
  roleCapabilityPredicates[]
  acceleratorPolicies[]
  executionDisposition: ACCEPT_IF_EXACT_REUSE | ACCEPT_WITH_PREPARATION | REJECT
  preparationAccepted
  residencySummary
  queueEstimate
  networkEstimate
  signature
}
```

The offer is a signed capability/willingness snapshot and performs no resource
reservation. `preparationAccepted=false` does not mean a negative protocol ACK:
the Provider may use `ACCEPT_IF_EXACT_REUSE` to offer only already verified,
role/rank-ready assembled artifacts or compatible loaded runtimes. It is
selectable only when the sealer proves that the proposed `RoleAssemblySpec`
exactly matches one such residency proof. `ACCEPT_WITH_PREPARATION` additionally
allows missing canonical layers to be fetched and a new assembled artifact to
be built after Selection. `REJECT` is the only negative protocol ACK and is
never selectable. Large topology/inventory details may be fetched by digest and
verified before planning; the bounded ACK summary alone cannot establish an
exact cache hit without its proof path.

The wire-level ACK `status` remains `true` for both selectable dispositions and
`false` for `REJECT`. This separation is mandatory: selecting a Provider after
an overall `status=false` would bypass authenticated willingness, contradict the
generic Collaboration API, and make timeout/replay behavior ambiguous. Existing
wire implementations may carry the new disposition and
`preparationAccepted` value in the signed V3 ACK payload; they must not reinterpret
the generic status bit.

Only these wire tuples are valid:

```text
status=true,  executionDisposition=ACCEPT_IF_EXACT_REUSE,
              preparationAccepted=false
status=true,  executionDisposition=ACCEPT_WITH_PREPARATION,
              preparationAccepted=true
status=false, executionDisposition=REJECT,
              preparationAccepted=false
```

Every other combination is a malformed ACK rejected by the trusted offer
validator before ACK_CLOSED/strategy input. The redundancy is intentional for
wire diagnostics and migration clarity; it never creates a fourth state.

### Exact-cache request example

The common warm-request case is not a negative ACK. If a Provider already has
the exact role/rank `.ndnsf-onnx-artifact` (or a compatible loaded runtime), but
does not agree to fetch or assemble any new artifact, it publishes:

```text
status=true
executionDisposition=ACCEPT_IF_EXACT_REUSE
preparationAccepted=false
residencyProof=<signed exact role/recipe/object/device evidence>
```

After `ACK_CLOSED`, the strategy may select that Provider only when the proof
matches the sealed role, graph, adapter, recipe, rank, and object digest. The
Provider then accepts the final Selection into its bounded queue, pins the
verified local bundle, and proceeds through just-in-time device admission; no
model fetch, assembly, or ACK-time GPU reservation occurs. A Request therefore
remains valid and can execute even though `preparationAccepted=false`.

Conversely, a Provider that has only reusable canonical layers, but no exact
assembled role artifact, must advertise `ACCEPT_WITH_PREPARATION` before it can
be selected for a path that assembles that role. Raw layer presence alone never
licenses the Provider to silently change the sealed role or recipe. Only
`status=false` with `REJECT` means that the Provider cannot be selected.

For V3 the serialized offer sets `ackReservation=false` and contains no
`reservationId`, `reservationLease`, queue ticket, device lease, or admission
fencing token. The Python Provider MUST bypass the current negotiated-reservation
attachment branch for V3. The branch remains reachable only for an explicit V2
compatibility request. ACK generation is observational: resource probing and
signature generation may update telemetry caches but not workload/resource
ownership state.

Zero accelerator devices is valid. A GPU-required role must not receive a
positive feasibility result from such an offer.

## Provider Planning View V3

The trusted offer validator derives a sanitized immutable view for strategy use:

```text
ProviderPlanningViewV3 {
  providerName
  serviceName
  bootEpoch
  offerDigest
  offerExpiry
  topologyProfileDigest
  resourceSnapshotDigest
  resourceSequence
  offeredDeviceViews[]
  roleCapabilityPredicates[]
  executionDisposition
  preparationAccepted
  residencyEvidenceViews[]
  queueEstimate
  networkEstimate
}
```

It exposes only verified planning facts, offer-scoped device handles, and the
digests needed for a proposal. Raw wire blocks, signatures, proof objects,
certificate/private material, live Provider objects, and runtime handles remain
inside the trusted validator/sealer boundary. `PlanSealerV3` resolves every
proposed reference back to the exact signed offer and proof path.

## Placement Strategy Port

```text
ModelPlacementStrategyV3.propose(PlacementRequestV3)
  -> PlacementProposalV3
```

`PlacementRequestV3` contains only immutable/canonical values:

- public request/attempt/deadline;
- exact model, artifact-profile, graph, and adapter descriptors;
- certified pipeline/tensor/redistribution recipes;
- ACK_CLOSED digest and validated, sanitized `ProviderPlanningViewV3[]`;
- canonical layer catalog and bounded exact residency evidence;
- task options and accelerator requirement.

It contains no live Provider object, file handle, model instance, CUDA context,
or strategy-supplied executable code.

`PlacementProposalV3` is an untrusted declarative candidate containing:

- pipeline stages with independent `tensorDegree M_i`;
- logical roles and rank assignments;
- Provider-local role bundles that group ranks whose device handles share one
  Provider offer namespace;
- Provider and device bindings;
- role assembly specifications;
- normal dependencies, collective groups, and redistribution edges;
- per-device resource/transfer estimates;
- fallback/replan ordering where permitted;
- decision evidence and deterministic strategy descriptor digest.

The proposal contains only references and declarative values. It cannot contain
a wire Selection, a pre-authorized admission result, an executable assembler,
or a Provider-local runtime object.

### Strategy implementation trust

V3 custom strategy implementations are operator-installed code trusted with
respect to the Requester process. The public request API never loads strategy
code from request payloads, Repo objects, ACKs, or other network content.
Execution is bounded by the invocation's planning deadline and candidate budget;
exception, timeout, cancellation, or budget exhaustion produces a classified
planning failure or an explicitly permitted built-in fallback and publishes no
Selection from the failed proposal.

The strategy output is untrusted even when its implementation is operator-
trusted. An adversarial/untrusted plugin execution model is outside this profile
and requires a future explicit out-of-process sandbox/resource/I/O policy.

## Trusted Plan Sealing

```text
PlanSealerV3.sealCore(PlacementRequestV3, PlacementProposalV3)
  -> PlacementPlanCoreV3
PlanSealerV3.grantView(PlacementPlanCoreV3, providerName)
  -> ProviderGrantViewV1
PlanSealerV3.finalizeSecurity(PlacementPlanCoreV3,
                              GrantBindingV1[],
                              securityPolicySnapshotDigest)
  -> PlacementPlanV3
PlanSealerV3.project(PlacementPlanV3, providerName)
  -> ProviderSelectionProjectionV3
```

`PlanSealerV3` is owned by NDNSF-DI, not by the external strategy. It resolves
all proposal references against the exact certified graph/adapter catalogs and
validated ACK_CLOSED offer set, canonicalizes ordering, and independently
checks:

- graph/layer/tensor coverage and legal stage boundaries;
- logical-role, rank, Provider-local bundle, and collective completeness;
- per-tensor distribution and every required layout redistribution;
- offered device ownership, topology, freshness, and per-device envelopes;
- acyclic authenticated dependencies, request/attempt/plan bindings, and
  deterministic terminal ownership;
- absence of opaque executable or pre-authorized Provider state.

`sealCore` canonicalizes every assignment, offer, graph, assembly, dependency,
resource, and protection requirement into `canonicalPlanCoreBytes` and
`planCoreDigest`; it deliberately contains no grant reference. After the
Requester obtains the complete Provider-specific grant cover,
`finalizeSecurity` sorts the non-secret `(provider, grantName, grantDigest)`
bindings, binds the security-policy snapshot, and derives:

```text
planDigest = H(canonicalPlanCoreBytes
               || canonicalSortedGrantBindings
               || securityPolicySnapshotDigest)
```

An unprotected plan uses an empty grant-binding list. Only a successfully
finalized plan may be projected into authenticated final Selection messages.
Every projection carries `planCoreDigest`, final `planDigest`, the policy
snapshot digest, and exactly its Provider's grant name/digest when protected.
A strategy can optimize placement but cannot weaken the protocol, security,
admission, or completeness invariants.

`ProviderGrantViewV1` is not a Selection and grants no execution authority. It
is a deterministic projection of the sealed core containing request/attempt,
`planCoreDigest`, selected Provider identity/certificate, exact offer digest,
model manifest, protection epoch/profile, allowed-residency request, role/
assembly digests, deadline, and policy-authority identifier. It contains no
wrapped key, grant reference, final `planDigest`, device admission result, or
executable object. `GrantRequestV1` binds its digest; the authority independently
resolves the signed core/offer references before issuing a grant.

## Device Binding

```text
DeviceBinding {
  mode: CPU | SINGLE_DEVICE | DEVICE_SET
  provider
  providerLocalBundle
  localRankTargets[]
  offerDigest
  topologyProfileDigest
  resourceSnapshotDigest
  resourceSequence
  atomicAdmissionGroup
  sharingPolicy
}

RankAssignment {
  rank
  providerLocalBundle
  offerScopedDeviceHandle?
  assemblySpecDigest
  tensorDistributionDigest
  resourceEnvelope
  collectiveGroup?
}
```

Global and local relationships are:

```text
LogicalRole
  -> RankAssignment[]
  -> ProviderLocalRoleBundle[]
       -> DeviceBinding
```

A cross-Provider logical role cannot own one global `DeviceBinding`, because an
offer-scoped device handle is meaningful only within its Provider's signed offer.

Rules:

- `CPU` has no accelerator handle and requires adapter/task CPU permission.
- `SINGLE_DEVICE` has exactly one device and one local rank for that binding.
- `DEVICE_SET` has a complete ordered member/rank set and is admitted atomically.
- every handle must occur in the exact bound Provider offer/profile/snapshot;
- physical runtime locators such as `cuda:0` are not stable wire identities;
- several independent role bindings may select distinct devices under one
  Provider;
- Provider memory is never pooled to satisfy one unsplittable device peak.

## Selection and Admission

Final Selection carries one Provider-specific opaque V3 projection containing
all `ProviderLocalRoleBundle` objects for that Provider. On receipt the Provider:

1. verifies request, attempt, ACK_CLOSED, plan, offer, topology, snapshot, and
   Selection authenticity;
2. validates model/adapter/assembly declarations, hard bounds, queue policy, and
   complete Provider-local cover;
3. atomically creates one bounded queue record for the complete projection or
   rejects it; this record contains no device reservation or capacity lease;
4. if the sealed path is exact reuse, pins the verified catalog entry without
   new fetch/assembly; otherwise it first verifies that the bound offer used
   `ACCEPT_WITH_PREPARATION`, then resolves manifests, fetches/verifies missing
   canonical objects, and assembles host-side under separate bounded
   disk/RAM/network preparation leases;
5. when the queue policy permits device work, re-probes/revalidates device
   visibility, health, resource sequence, sharing/failure domain, and complete
   per-device phase envelope;
6. atomically acquires every required local device resource and emits one
   monotonically increasing `admissionFencingToken`, or acquires nothing;
7. loads, reaches local-ready, and executes only while that fencing token remains
   current.

The state machine is:

```text
SELECTION_RECEIVED
  -> SELECTION_VALIDATED
  -> QUEUE_ACCEPTED                 # no GPU/device hold
  -> HOST_PREPARING
  -> HOST_READY
  -> DEVICE_ADMISSION_PENDING
  -> DEVICE_ADMITTED(fencingToken)  # atomic complete local vector
  -> LOADING
  -> LOCAL_READY
  -> EXECUTING
  -> COMPLETED
```

Before `DEVICE_ADMITTED`, cancellation removes the queue/preparation record and
releases only disk/RAM preparation state. Queue deadline expiry becomes
`QUEUE_EXPIRED`; stale topology/capacity at admission becomes
`REPLAN_REQUIRED`; policy rejection becomes `SELECTION_REJECTED`; preparation
failure becomes its narrow lifecycle class. After admission, cancellation,
device loss, or epoch failure fences the token, aborts the complete affected
local group, releases every member resource, and reports one terminal outcome.
No state may hold a strict subset of a `DEVICE_SET`.

A retained loaded runtime may consume evictable cache memory between requests,
but it is not an exclusive per-request reservation and confers no execution
slot. Reuse still passes queue acceptance and JIT admission, which validates the
runtime/device fence and atomically acquires the execution resource vector.
Canonical layers and assembled ONNX files follow the bounded disk-cache policy;
container scratch is removed at exit, while any cross-container cache requires
an explicit bounded persistent mount.

Each bundle executes independently when local-ready and its own authenticated
dependencies are ready. Queue acceptance and device admission are not global
model-ready barriers and no second execution-start message exists.

Any mismatch fails the exact assignment. The Provider must not select another
device, change rank, or weaken the accelerator requirement. Replanning creates a
new attempt/plan under the original public request ID.

## Strategy Extension Contract

External strategies implement the same pure declarative port. They may choose
among adapter-certified recipes but cannot introduce slicing code, collective
implementations, device probes, or executable artifacts.

The default `LayerReuseFirstStrategy` ranks only feasible decisions, then prefers:

1. exact compatible loaded runtime on the exact device set;
2. exact assembled fragment in host memory/disk;
3. verified canonical layer/profile reuse;
4. minimum missing bytes, queue/transfer/assembly/load cost, and resource risk.

The default is selected in the public Application path. Passing an external V3
strategy replaces only the pure proposal function; it does not replace canonical
publication, offer validation, plan sealing, Selection projection, Provider
queue/admission, or security enforcement.

## Native and Exact-SIF Offer Wiring

The native path MUST be behaviorally equivalent to the Python path:

```text
ProviderResourceProbe
  -> NativeProviderReadinessState::makeAckDecision
  -> V3 offer encoder
  -> generic NDNSF collaboration ACK
  -> validated ProviderPlanningViewV3
```

`NativeProviderReadiness.hpp/.cpp`, `ProviderResourceProbe.hpp/.cpp`, the native
Provider executable/entry point, unit-test build list, install manifest, and SIF
recipe MUST all include the V3 encoder and real runtime probe. A fixture that
constructs offer JSON without this path is contract-test evidence only and cannot
satisfy native/SIF parity. Exact-SIF validation compares Python/native canonical
offer bytes after removing only signature-format differences explicitly allowed
by the schema; topology/profile/snapshot and `ackReservation=false` fields must
match semantically.

## V2 Compatibility

- V2 means one preassembled role artifact and one non-CPU device.
- V2 offers/assignments cannot be projected into CPU, multi-device, or
  heterogeneous-rank V3 semantics.
- V2 and V3 digests, cache keys, and loaded-runtime identities are disjoint.
- mixed V2/V3 collective or device-set plans fail before final Selection.
- V2 is an explicit supported compatibility profile for Spec 170, not an
  automatic fallback and not a temporary path with an unspecified deletion date.
- V2 use emits a profile-specific counter and evidence field. Removal requires a
  future breaking-change spec that inventories callers and migration evidence.
