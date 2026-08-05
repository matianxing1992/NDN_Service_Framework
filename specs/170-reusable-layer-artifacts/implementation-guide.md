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
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py` | Creates `PreSplitFirstStrategy` when no strategy is provided | Normal calls create `LayerReuseFirstStrategy`; explicit V2 profile creates PreSplitFirst |
| `.../app_sdk/client.py` | Builds current split materializer/publisher coordinator | Dispatch V3 canonical ensure versus explicit V2 role-split preparation |
| `.../app_sdk/placement.py::_prepare_artifacts()` | Resolves/materializes/publishes the selected role split | V3 calls canonical ensure and seals `RoleAssemblySpec`; it never materializes a role split |
| `.../provider.py::attach_negotiated_reservation()` | Can attach reservation lease during ACK | V3 bypasses this function; V2 behavior is retained only behind explicit profile dispatch |
| `.../planner/presplit_first.py` | Current default placement strategy | Legacy V2-only strategy with profile-specific telemetry |
| `.../planner/layer_reuse_first.py` | New implementation target | Default pure V3 proposal strategy over sanitized immutable planning views |
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
    strategy = explicit_strategy or LayerReuseFirstStrategy()
    validate strategy is a V3 proposal port
    coordinator = V3PlacementCoordinator(
        canonicalPublisher, offerValidator, planSealer, strategy)
elif placement_profile == PREASSEMBLED_PARTITION_SINGLE_DEVICE:
    reject a V3-only strategy/option
    coordinator = legacy V2 coordinator using PreSplitFirstStrategy
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

## 9. Cross-Provider NDNSF_DATA_V1

Control and payload identity are carried by `GroupCapabilityV1`, a signed
`CollectiveOperationManifestV1`, and immutable AEAD-encrypted/HMAC-signed
segments. HKDF derives a per-operation key; the unique nonce binds capability,
epoch, operation, producer, and segment; the full Data name and manifest digest
are associated data. Consumers expose a tensor to a collective/redistribution
operator only after complete
manifest/bitmap/digest verification. An identical duplicate is idempotent; a
same-name different-byte duplicate is an integrity/replay failure.

Bound every operation by permitted peers/operations, total bytes, segment count,
segment size, inflight bytes, no-progress time, and hard deadline. Cancellation
stops fetch, drops incomplete plaintext, releases local admission, and emits no
downstream readiness. A new peer/member requires a new plan/group epoch/key.

Provider-local collectives may use a certified local backend. Cross-Provider raw
NCCL/socket/RDMA payloads are not Spec 170 evidence.

## 10. D2h Frozen Rank Mapping

Two Provider processes each see exactly one GPU:

| Vector | P0/G0 | P1/G1 |
|---|---|---|
| `[1,2,1]` | `S0R0`, `S1R0` | `S1R1`, `S2R0` |
| `[2,1,2]` | `S0R0`, `S1R0`, `S2R0` | `S0R1`, `S2R1` |

Co-resident ranks are one plan-local `EXCLUSIVE_PLAN` vector. Sum their
phase-specific weight/replication/activation/KV/collective/transient peaks before
admission. This is neither MPS nor multi-tenant sharing. If the vector does not
fit, D2h is `BLOCK`; do not remap after seeing hardware.

## 11. Freeze Discipline

T029 is the only formal freeze. Before it, finish all source/security/build/
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
  tests/python/test_spec170_canonical_artifacts.py \
  tests/python/test_spec170_provider_assembly.py \
  tests/python/test_spec170_residency_reuse.py \
  tests/python/test_spec170_content_addressed_reuse.py \
  tests/python/test_spec170_multi_device_provider.py \
  tests/python/test_spec170_hybrid_execution.py \
  tests/python/test_spec170_artifact_security.py \
  tests/python/test_spec170_real_minindn_gate.py
```

Native unit tests, package tests, MiniNDN, exact SIF, and TigerCluster commands
are closed by their tasks and evidence manifests; a source grep or fixture-only
test never substitutes for an executed runtime path.

## 13. Definition of Implemented

Use evidence labels precisely:

- `implemented`: source exists;
- `wired`: public/native/package call path reaches it;
- `executed`: the real path ran;
- `measured`: the frozen result artifact contains the declared metric;
- `PASS`: every mapped FR/SC/H gate passes against the same frozen identity.

No implementation-complete claim is valid until `traceability.md` and the final
evidence traceability contain no missing link.
