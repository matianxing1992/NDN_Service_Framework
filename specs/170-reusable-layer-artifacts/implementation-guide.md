# Implementation Guide: Spec 170

**Audience**: An implementation agent that has repository access but no access
to the design conversation. This guide is subordinate to `spec.md` and the three
contracts; if prose conflicts, the normative FR/SC and contract text wins.

## 1. Required Outcome

Replace the normal NDNSF-DI pre-split Requester workflow with a V3 workflow in
which the Requester publishes one placement-independent canonical model/layer
representation, a strategy plans from immutable ACK_CLOSED evidence, and each
selected Provider fetches/assembles/loads its own exact role. Preserve the old
pre-split behavior only as an explicit V2 compatibility profile.

The implementation is incomplete if it merely adds new classes or tests. The
public Application default, Python Provider, native Provider executable, package
build/install path, real MiniNDN harness, and exact SIF must all use V3.

## 2. Current Source Reality and Required Edit Points

| File/symbol | Current behavior to replace or branch | Required V3 behavior |
|---|---|---|
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py` | Later Spec170 work redirected normal V3 calls to `LayerReuseFirstStrategy` | Normal V3 calls create `PreSplitFirstStrategy`; reuse is subordinate scoring; explicit V2 uses only the isolated role-artifact materializer |
| `.../app_sdk/client.py` | Builds current split materializer/publisher coordinator | Dispatch V3 canonical ensure versus explicit V2 role-split preparation |
| `.../app_sdk/placement.py::_prepare_artifacts()` | Resolves/materializes/publishes the selected role split | V3 calls canonical ensure and seals `RoleAssemblySpec`; it never materializes a role split |
| `.../provider.py::attach_negotiated_reservation()` | Can attach reservation lease during ACK | V3 bypasses this function; V2 behavior is retained only behind explicit profile dispatch |
| `.../planner/presplit_first.py` | Existing candidate strategy | Default V3 ACK-driven, adapter-certified candidate generator with one-to-one role/Provider ownership |
| `.../planner/layer_reuse_first.py` | Later Spec170 topology owner | Subordinate feasible-plan cost scorer only; it must not own role topology |
| `.../cpp/ndnsf-di/NativeProviderReadiness.cpp::makeAckDecision()` | Native readiness ACK source | Encode the same V3 offer/no-reservation facts as Python |
| `.../cpp/ndnsf-di/ProviderResourceProbe.*` | Existing runtime resource probe | Produce stable topology plus mutable snapshot and exact offer-scoped handles |
| `examples/DI_NativeProviderExecutable.cpp` | Installed native Provider caller | Register real probe/readiness/V3 handler; no fixture defaults |
| `examples/wscript`, `packaging/ndnsf-di-container/oci/Dockerfile.gpu`, and `packaging/ndnsf-di-container/oci/layered/Dockerfile.app` | Build/install source lists | Include every new V3 native source and installed runtime asset |

Never edit generated `build/lib` or packaging build-copy files. Rebuild from the
owning source and verify generated copies/hashes through the normal build.

## 3. Public Dispatch Contract

The public API remains intent-only:

```python
handle = application.request(
    model=ModelRef(human_name, immutable_model_digest, artifact_profile),
    task=TaskRef(adapter_contract, accelerator_requirement),
    input=payload,
    options=inference_options,
    deadline=deadline,
    strategy=optional_v3_strategy,
)
```

Dispatch pseudocode:

```text
if placement_profile is absent or DI_PLACEMENT_V3:
    strategy = explicit_strategy or PreSplitFirstStrategy(reuse_cost_scorer)
    validate strategy is a V3 proposal port
    coordinator = V3PlacementCoordinator(
        canonicalPublisher, offerValidator, planSealer, strategy)
elif placement_profile == PREASSEMBLED_PARTITION_SINGLE_DEVICE:
    reject a V3-only strategy/option
    coordinator = isolated legacy V2 role-artifact coordinator
else:
    fail configuration before Request
```

There is no `try V3 -> catch -> use V2`. V3 planning, sealing, publication,
queue, preparation, or execution failure remains a V3 failure/replan.

## 4. V3 Requester Algorithm

```text
create public requestId once
publish generic Request
collect and validate signed ProviderOfferV3 values
close immutable ACK_CLOSED snapshot
resolve pinned model manifest, canonical profile, graph, and adapter recipes
call ModelPlacementStrategyV3.propose(sanitized immutable request)
seal immutable placement/security core with PlanSealerV3.sealCore
ensure missing canonical model/layer manifests/objects only
for protected Providers, acquire complete KeyGrant cover bound to planCoreDigest
finalize planDigest from core + sorted grants + security-policy snapshot
project finalized plan once per selected Provider
publish authenticated final Selections
observe progress/dependencies/terminal Response under same requestId
```

Canonical ensure may run early only as idempotent placement-independent
prepublication. It must not know final stage boundaries, ranks, Providers, or
device bindings. `RoleAssemblySpec` is declarative Selection data.

## 5. Provider Offer and ACK Algorithm

```text
probe actual container-visible CPU/RAM/storage/devices
apply AUTO/NONE/EXPLICIT_SUBSET as a restricting filter
construct DeviceTopologyProfile
construct DeviceResourceSnapshot(resourceSequence++)
construct bounded residency/capability summaries and proof references
choose ACCEPT_IF_EXACT_REUSE, ACCEPT_WITH_PREPARATION, or REJECT
sign ProviderOfferV3(ackReservation=false, preparationAccepted=...)
return ACK
```

Use generic ACK `status=true` for the first two dispositions and `status=false`
only for `REJECT`. A Provider that declines new model preparation therefore
offers `ACCEPT_IF_EXACT_REUSE`; the sealer may select it only if an exact
assembled role/rank artifact or compatible loaded-runtime proof matches the
proposed `RoleAssemblySpec`. Never select an overall negative ACK.

Forbidden V3 ACK effects:

- calling `attach_negotiated_reservation()`;
- creating reservation, device lease, admission fence, or queue ticket;
- decrementing available capacity or changing workload ownership;
- exposing a device outside the scheduler/container-visible set;
- exposing raw cache proofs, secrets, or runtime handles to the strategy.

Probe/telemetry caches may update because they describe observations, not
ownership.

## 6. Queue and Just-in-Time Admission Algorithm

Selection-time queue acceptance and device admission are separate atomic
transactions.

```text
on Selection projection:
  authenticate and bind request/attempt/ACK_CLOSED/plan/offer
  validate complete local bundles, hard bounds, queue policy
  if exact reuse: revalidate and pin the matching catalog entry
  else: require bound offer disposition ACCEPT_WITH_PREPARATION
  atomically append one QueueAcceptanceRecord for the whole projection
  # no GPU/device bytes or lease acquired here

while queued:
  verify/fetch canonical content under bounded disk/RAM preparation leases
  assemble and atomically activate host fragment

when queue head/policy permits device work:
  re-probe visibility/health/profile/snapshot/resourceSequence
  recompute complete phase-specific per-device vector
  atomically acquire all members or none
  issue monotonic admissionFencingToken
  load and execute only while token is current
```

Terminal mapping:

| Condition | Required state/evidence |
|---|---|
| Queue full/policy refusal | `SELECTION_REJECTED`, no queue/device state |
| Queue deadline before admission | `QUEUE_EXPIRED`, remove queue/preparation leases |
| Cancel before admission | `CANCELLED_BEFORE_ADMISSION`, no device release event claimed |
| Stale/lost device at admission | `REPLAN_REQUIRED`, no partial device hold |
| Partial device-set availability | `ADMISSION_REJECTED`, acquire none |
| Cancel/loss after admission | fence token, abort complete affected local group, release all members |
| Stale token on load/execute/release | reject operation and retain offending/current token evidence |

Host-side assembly must not hold a GPU while Repo transfer makes progress.

## 7. Canonical Artifact Implementation

Only this name grammar is valid:

```text
/<publisher>/NDNSF-DI/MODEL/v1/NAME/<name...>
  /MID/<model-identity-digest>/PROFILE/<profile-digest>
  /MANIFEST/<model-manifest-digest>
  /LAYER/<kind>/<coordinate>/MANIFEST/<layer-manifest-digest>
  /OBJECT/<object-digest>/<segment-number>
```

Publication order is objects → layer manifests → transformation attestation →
ACTIVE root manifest. Equality excludes request/attempt/Provider/role/stage/
rank/strategy. Different placement plans must resolve the same canonical bytes.

Provider assembly key includes model/profile, `RoleAssemblySpec`, adapter/
assembler, ABI, precision/quantization, and protection epoch. Loaded-runtime key
adds exact ordered device set/topology, backend/driver/kernel, Provider boot/
process/runtime generation, admission fence, collective epoch, and reusable-state
contract.

The ONNX baseline writes exactly one durable content-addressed
`.ndnsf-onnx-artifact` bundle with embedded signed manifest for each complete
role/rank assembly. It carries inline `model.onnx` when safely bounded, or
`model.onnx` plus one colocated external-data entry for a large model. Catalog
it under
`/<provider>/NDNSF-DI/ASSEMBLED/v1/NAME/.../MID/.../PROFILE/.../GRAPH/.../ROLE/<kind>/<semantic-coordinate...>/RANK/.../RECIPE/.../OBJECT/...`
and map the final object digest to a safe content-addressed local bundle file.
The semantic role coordinate uses canonical layer indices/ranges or an ordered
component-set digest and excludes request/attempt IDs, arbitrary stage labels,
and filenames. Verify entry count, total/expanded bytes, per-entry digest, and
path safety before atomically materializing the runtime files into private
container scratch; large models are checked by path with external data
colocated. The durable cache remains one bundle file.
Canonical layers and assembled files persist across requests under bounded
cache policy. Container scratch is deleted at exit; cross-container reuse exists
only when the operator mounts an explicit bounded persistent cache volume.

The implementation represents these levels with
`CanonicalResidencyIdentity`, `AssembledFragmentIdentity`, and
`LoadedRuntimeIdentity`. `ProviderResidencyLedger` keeps separate bounded LRU
inventories and active-request owner fences for them. `BoundedSingleFlight`
keys FETCH by canonical object identity, BUILD by assembly-spec identity, and
LOAD by loaded-runtime identity. `ExactArtifactPreparationPipeline` rechecks
the ledger before and inside each flight, so equal concurrent requests perform
each operation once. Only an exact loaded-runtime hit reports zero transferred,
built, and loaded model bytes. Boot/process/runtime generation, topology,
fencing-token, or protection-epoch changes invalidate the affected identities;
an owned identity cannot be invalidated or evicted.

## 8. Protected Profile Implementation

After core sealing and before final plan sealing/Selection, the Requester sends a signed
`GrantRequestV1` to the configured artifact policy authority. The authority
validates requester/model authorization, Provider certificate, sealed
`ProviderGrantViewV1`/core/offer, protection policy, and current
`RevocationStateV1`, then
returns signed Grant Data encrypted to that Provider. Selection contains only
its name/digest. The grant is bound to Provider/request/attempt/plan core/model/
protection epoch/residency tiers/expiry/revocation sequence; failure to acquire
every selected grant publishes no Selection.

After obtaining the complete grant cover, call
`finalizeSecurity(core, canonicalSortedGrantBindings,
securityPolicySnapshotDigest)`. The final `planDigest` hashes the canonical core,
the sorted non-secret Provider grant name/digest bindings, and that snapshot.
This ordering prevents a circular `grant <-> planDigest` dependency.

Implementation order:

1. verify grant signature, recipient, all plan bindings, time, and revocation;
2. register plaintext host allocation before decrypting into it;
3. register device allocation and admission fence before device copy/decrypt;
4. check grant/fence before reuse and every new operation;
   resolve signed revocation state before JIT admission/reuse and no later than
   its `nextCheckAt`; stale/unreachable state past that time fails closed;
5. on expiry/revocation/rotation/restart/loss, fence runtime and cancel active use;
6. zero host buffers; zero and synchronize device buffers or destroy/fence the
   context when overwrite cannot be proven;
7. record `ZEROIZED` before removing registry entries.

Encrypted canonical objects may remain cached. Plaintext fragments or loaded
runtimes may not survive revocation. Zeroization failure is `FAILED_CLOSED` and
must prevent reuse.

## 9. Cross-Provider consumer-pull NDN dataflow

Each Provider projection carries one `RoleDataflowContract`. A consumer expresses
Interests only for entries in `mustFetch[]`; a producer answers only names in
`mayPublish[]`; `waitFor[]` becomes ready only after full verification. Control
and payload identity are carried by `GroupCapabilityV1`, a signed
`TensorObjectManifestV1`, and immutable AEAD-encrypted/HMAC-signed segments.
The endpoint advertised in the ACK is the routable Provider identity namespace,
not its SVS node ID. Providers register `/<provider>/NDNSF-DI`; each exact Data
packet is identity-signed, and consumers validate both the trust schema and the
expected Provider identity before decoding content.

When one produced tensor feeds multiple roles, Core seals one shared object
identity and the manifest carries the complete authorized consumer-role set.
Each consumer has a local `mustFetch` projection to that same exact name; the
producer has one `mayPublish` entry and publishes the object only once.
HKDF derives a per-operation key; the unique nonce binds capability, epoch,
operation/round, producer role/rank, microbatch, and segment; the full Data name
and manifest digest are associated data. Consumers expose a tensor to a
collective/redistribution operator only after complete
manifest/bitmap/digest verification. An identical duplicate is idempotent; a
same-name different-byte duplicate is an integrity/replay failure.

Bound every operation by permitted peers/operations, total bytes, segment count,
segment size, inflight bytes, no-progress time, and hard deadline. Cancellation
stops fetch, drops incomplete plaintext, releases local admission, and emits no
downstream readiness. A new peer/member requires a new plan/group epoch/key.

Retransmission re-expresses the same Interest name. Cross-Provider raw
NCCL/socket/RDMA/RPC payloads and shared files are forbidden.

The current deterministic gates cover exact-name round trips, concrete manifest
codec/mutation rejection, V3-authoritative runtime edges, identity-signed exact
Interest/Data with same-name retry and cancellation, wrong-signer rejection,
and complete manifest-first multi-segment reconstruction. Before a role runner
is invoked, `AsyncDataflowRuntime` and `ProviderRoleWorker` independently check
that every sealed V3 input has nonempty reconstructed payload, exact byte count,
and a concrete segment count within the signed bound. The worker stages and
validates every declared output before publishing any output, so a missing or
invalid later output cannot expose an earlier partial result. Legacy SVS
DATA_V1 tests remain only as compatibility coverage; V3 execution uses the
exact consumer-pull path. Network publication is not claimed to be a
cross-object transaction; downstream readiness remains protected because no
consumer receives a tensor until its own complete object verifies.

## 10. Hybrid Role Mapping

Use one distinct Provider per stage/rank role. `[1,2,1]` therefore needs four
Providers for `{S0R0,S1R0,S1R1,S2R0}`; `[2,1,2]` needs five for
`{S0R0,S0R1,S1R0,S2R0,S2R1}`. Freeze the role/Provider map before execution and
reject any same-Attempt Provider reuse. CPU integrated/MiniNDN is the first
correctness gate; a GPU campaign runs only when equivalent Provider resources are
available and never compresses roles to fit an allocation.

## 11. Freeze Discipline

T022 is the only formal freeze. Before it, finish all source/security/build/
install/harness changes, model/artifact preparation, Gate A, real MiniNDN Gate B,
exact-SIF Gate C, mutations, and traceability. The freeze manifest binds hashes
and timestamps. After it, run jobs and write evidence/docs only.

Any post-freeze executable or workload mismatch yields `INVALID_CANDIDATE` and
routes back to the owning task. Never patch a remote run in place, rebuild SIF,
or reprepare model payload while retaining the same candidate ID.

## 12. Minimum Verification Commands

The task that creates each planned test must make these commands real:

```bash
speckit-audit specs/170-reusable-layer-artifacts --strict

python3 -m pytest -q \
  tests/python/test_spec170_placement_v3.py \
  tests/python/test_spec170_runtime_topology.py \
  tests/python/test_spec170_ack_no_reservation.py \
  tests/python/test_spec170_admission_lifecycle.py \
  tests/python/test_spec170_default_application_path.py \
  tests/python/test_spec170_canonical_layers.py \
  tests/python/test_spec170_provider_assembly.py \
  tests/python/test_spec170_layer_reuse_first.py \
  tests/python/test_spec170_content_addressed_reuse.py \
  tests/python/test_spec170_multi_device_provider.py \
  tests/python/test_spec170_hybrid_execution.py \
  tests/python/test_spec170_artifact_security.py \
  tests/python/test_spec170_real_minindn_gate.py
```

Native unit tests, package tests, MiniNDN, exact SIF, and TigerCluster commands
are closed by their tasks and evidence manifests; a source grep or fixture-only
test never substitutes for an executed runtime path.

### 12.1 Current local-SIF/TigerCluster boundary

The reusable operator procedure for updating NDNSF-DI and producing a new SIF
is the "Source-to-SIF update contract" in
[`docs/NDNSF-DI-runtime-workflow.md`](../../docs/NDNSF-DI-runtime-workflow.md).
Use it before changing the source, lock, definition, or native bindings; do
not repair an existing SIF in place.

Before source sealing, also generate
`candidate-input-inventory.json` with
`tools/ndnsf-di/collect_spec170_candidate_inputs.py`. This deterministic
pre-freeze inventory prevents a source-only seal from silently omitting a
build script, experiment entry point, harness, test, or Spec170 contract. It
does not replace T022's frozen manifest or the separate model, schedule,
security, Gate A/B/C, and exact-SIF evidence records.

The current operational route is: build one complete application SIF locally
with Apptainer, compile all container-bound native/Python extensions inside the
SIF build stage or an ABI-identical sealed builder, run the full target-closure
and exact-SIF/MiniNDN gates, then copy one hash-bound SIF to project storage.
TigerCluster verifies, stages once, and executes; it does not build, pull
Docker/OCI, materialize, or repair the candidate. The last bounded-verification
snapshot is r23 (Apptainer 1.5.3, SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`) with
`containerNativeBuild=true` and `hostBinaryInputs=[]` in its build record.
The r23 image is usable for sealed revision
`989a9daace669a4f93496dade3176c527edb2469`, but uncommitted working-tree
changes are not included in its source seal. It is therefore not eligible for
executing those changed bytes; a new candidate must be built before Tiger
execution.

Before a new candidate, audit every Waf target (including both ONNX smoke and
both Provider targets) for explicit `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL`
closure, reject stale base-image extensions and host RPATHs, require
`cd "$BUNDLE"` for relative artifacts, and isolate every process HOME/PIB.
Start Controller → bootstrap Provider → User when NAC-ABE DKEY setup is needed.
Do not create another multi-gigabyte SIF or model copy while local free space
is constrained; retain the active SIF, record, hashes, and canonical evidence.

When a framework C++ file or Python binding changes, rebuild the host MiniNDN
diagnostic stack before using its result: rebuild the
`ndnsf-service-framework` target, rebuild the in-place Python extension from
the `pythonWrapper` directory, check
the extension/framework hashes and `ldd`, then run the focused C++/Python and
real MiniNDN gates. This host rebuild is diagnostic-only. The resulting host
extension is never a SIF input; the candidate SIF must compile its own native
library and `_ndnsf.so` inside the sealed Python-3.10 builder and repeat the
exact-SIF closure checks. This distinction prevents stale host artifacts from
masking a source fix and prevents a successful host import from qualifying the
container image.

The latest bounded evidence is deliberately not over-claimed: two real CPU
MiniNDN positive hybrid cases passed after the host rebuild, and the delayed
post-certificate cancellation diagnostic now passes when its injected 5-second
role delay is paired with an explicit 12-second DATA_V1 no-progress bound.
The run rejects stale and late cancellation, preserves the accepted terminal
result, and accepts cleanup for the replacement epoch. The normal default
remains 2 seconds; 12 seconds is only for that injected-delay scenario. T022
and T019 remain open for the independent freeze and performance reasons, not
because this cancellation diagnostic is still failing.

## 13. Definition of Implemented

Use evidence labels precisely:

- `implemented`: source exists;
- `wired`: public/native/package call path reaches it;
- `executed`: the real path ran;
- `measured`: the frozen result artifact contains the declared metric;
- `PASS`: every mapped FR/SC/H gate passes against the same frozen identity.

No implementation-complete claim is valid until `traceability.md` and the final
evidence traceability contain no missing link.
