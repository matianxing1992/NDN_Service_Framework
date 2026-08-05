# Contract: Heterogeneous Pipeline/Tensor Execution V1

**Status**: Planned  
**Owner**: NDNSF-DistributedInference

## Topology

A hybrid plan is:

```text
Pipeline stages: S_0 ... S_(N-1)
Tensor degrees:  M_0 ... M_(N-1), each M_i >= 1
Total ranks:     sum(M_i)
```

There is no mandatory global tensor degree.

- `M_i = 1`: stage `i` is one unsplit pipeline role with no tensor collective.
- `M_i > 1`: stage `i` is one logical role implemented by `M_i` ranks.
- Collective-group count equals the number of stages whose degree exceeds one,
  unless an adapter explicitly certifies a more specialized grouping contract.

## Tensor Distribution Within a Sharded Stage

`M_i > 1` defines a participant group; it does not require every tensor to be
cut into `M_i` equal pieces. Each adapter-certified tensor rule is explicit:

```text
SHARDED(axis, rank, worldSize, padding, layout)
REPLICATED(memberSet)
OWNER_ONLY(rank)
LOCAL_DERIVED(recipeDigest)
```

The recipe must cover every required parameter/state/input/output exactly once
under its declared semantics. Unsupported tensors make the stage recipe
infeasible.

## Global and Provider-Local Assignment

```text
LogicalRole
  -> RankAssignment[]                 # global rank map
  -> ProviderLocalRoleBundle[]        # one subset per selected Provider
       -> DeviceBinding               # handles valid in that Provider offer
```

A device handle has meaning only inside its Provider offer/profile. Therefore a
global logical role spanning Providers cannot own one global `DeviceBinding`.
Final Selection projects all bundles for one Provider into one authenticated
Provider-specific assignment.

`ProviderLocalRoleBundle` fields:

- Provider, logical role, and local rank IDs;
- local CPU/device-set binding and per-rank resource envelopes;
- assembly specifications and tensor distributions;
- collective/redistribution endpoints;
- offer/profile/snapshot/sequence digests;
- atomic local admission group.

All local resources in one bundle are validated and admitted/enqueued together.

### Provider-local and cross-Provider groups

- A Provider-local tensor group places all ranks in one
  `ProviderLocalRoleBundle`, uses one ordered local device set, and admits that
  set atomically.
- A cross-Provider tensor group places ranks in two or more local bundles. Each
  Provider independently validates/admit its own bundle; no Provider can admit
  or address another Provider's device.
- Cross-Provider rendezvous binds peer identities, authenticated endpoints,
  request/attempt/plan/group/epoch, layouts, operation order, and failure rules.
- The collective becomes runnable only when every member of that group and its
  direct input are ready. This is group-local data readiness, not a global
  preparation barrier.
- Missing or lost membership fails the complete affected epoch. Replacement
  requires a new sealed plan generation.

## Cross-Provider Transport Profile: NDNSF_DATA_V1

Spec 170 chooses one mandatory cross-Provider payload profile; it does not leave
the transport as an implementation-time decision. Provider-local ranks may use
an adapter-certified local collective backend, but every cross-Provider
collective or redistribution edge uses authenticated NDNSF named segmented Data.

### Capability and epoch establishment

`PlanSealerV3` creates one `GroupCapabilityV1` per cross-Provider group:

```text
GroupCapabilityV1 {
  requestId / attemptId / planDigest
  groupId / epoch
  orderedMembers[] {provider, rank, offerDigest, endpointPrefix}
  permittedOperations[] {operationIndex, kind, producerRanks, consumerRanks,
                          tensorLayoutDigest, maxBytes, maxSegments}
  maxInflightBytes / noProgressMs / hardDeadline
  epochKeyId / wrappedEpochKeyByProvider[]
  capabilityDigest / sealerSignature
}
```

Every Provider-specific Selection carries the same capability digest and only
that Provider's wrapped epoch key. The key is encrypted to its Provider identity.
The endpoint prefix is an NDNSF-DI namespace, not a raw socket address. A peer
must validate Selection, group membership, offer binding, capability signature,
epoch, and local key unwrap before declaring rendezvous ready.

The trusted Requester-side sealer generates a fresh 256-bit epoch key from the
platform CSPRNG after the plan is sealed. The strategy, ACK, Repo, logs, and
evidence never receive it. The sealer wraps it independently to every selected
Provider certificate, retains only bounded retry state until Selection delivery
or the invocation hard deadline, then zeroizes its plaintext copy. A new plan or
group epoch always generates a new key; nonce/key reuse across epochs is invalid.
Provider plaintext keys live in the protected runtime registry and are zeroized
on completion, cancellation, expiry, restart, or epoch replacement.

### Operation manifest and Data names

The producer publishes a signed root manifest before consumers accept segments:

```text
CollectiveOperationManifestV1 {
  capabilityDigest / epochKeyId
  requestId / attemptId / planDigest / groupId / epoch
  operationIndex / operationKind / producerRank
  sourceLayoutDigest / targetLayoutDigest / tensorDigest
  totalBytes / segmentSize / segmentCount
  orderedSegmentDigests[]
  createdAt / noProgressMs / hardDeadline
  producerSignature
}

/<producer>/NDNSF-DI/COLLECTIVE/v1
  /REQ/<request-id>/ATTEMPT/<attempt-id>/PLAN/<plan-digest>
  /GROUP/<group-id>/EPOCH/<epoch>/OP/<operation-index>
  /RANK/<producer-rank>/TENSOR/<tensor-digest>
  /SEG/<segment-number>
```

Each segment content is AEAD-encrypted with a per-operation key derived from the
group epoch key by HKDF. Its nonce is uniquely derived from capability digest,
epoch, operation index, producer rank, and segment number; reuse is rejected.
The full Data name plus operation-manifest digest is AEAD associated data. The
NDN Data uses an epoch HMAC signature so a consumer avoids per-segment public-key
verification after validating the signed manifest/capability. Consumers verify
all identity components, declared bounds, AEAD tag, HMAC, ciphertext segment
digest, and complete bitmap before exposing plaintext to the collective/
redistribution operator. No partial tensor is a dependency-ready event.

### Bounds, replay, cancellation, and failure

- `maxBytes`, `maxSegments`, `segmentSize`, `maxInflightBytes`, operation count,
  and per-peer state are validated before allocation; overflow fails closed.
- An identical duplicate segment is idempotent. A duplicate name with different
  ciphertext/tag/HMAC, an already-completed operation with a different manifest,
  or any
  stale request/attempt/plan/group/epoch is a replay/integrity failure.
- Receivers maintain a bounded operation/segment bitmap until terminal epoch
  evidence is committed, then retain only the terminal digest/replay fence.
- Verified progress refreshes only the no-progress deadline; it never extends
  the sealed hard deadline. Missing progress cancels the whole affected epoch.
- Cancellation publishes authenticated terminal status, stops new fetches,
  discards incomplete plaintext, releases local admission, and prevents any
  downstream readiness event. Restart/replacement requires a new plan/group
  epoch and epoch key.
- Raw TCP, RDMA, or NCCL payload channels between Providers are out of scope for
  this baseline. Adding one requires a versioned transport contract with peer
  authentication, confidentiality/integrity, replay, bounds, cancellation, and
  matched evidence; it cannot silently replace `NDNSF_DATA_V1`.

## Intra-Stage Collective Contract

A stage with `M_i > 1` carries:

- group/member/rank and epoch identities;
- authenticated rendezvous and communicator compatibility;
- tensor layout before and after every ordered operation;
- all-reduce, all-gather, reduce-scatter, broadcast, or adapter-defined merge;
- input readiness, timeout, cancellation, and deterministic operation order;
- whole-group failure propagation and replan generation.

One missing/stale rank prevents group activation. A live epoch cannot silently
replace one member; replacement creates a new complete group/plan generation.

## Inter-Stage Redistribution

For boundary `S_i -> S_(i+1)`:

| Degrees | Required contract |
|---|---|
| `1 -> 1`, compatible layout | Normal authenticated pipeline dependency |
| `1 -> k` | Adapter-certified scatter, broadcast, or fan-out |
| `k -> 1` | Adapter-certified gather, reduce, or merge |
| `k -> l`, degree or layout changes | Adapter-certified reshard/layout transformation |

Each `RedistributionEdge` binds:

- producer/consumer stage and rank sets;
- source/target tensor layouts and shapes;
- exact operation/recipe digest;
- message/object integrity and request/attempt/plan/epoch identity;
- completion rule and downstream readiness condition;
- timeout, cancellation, duplicate/replay, and failure behavior;
- transfer and temporary-memory envelope.

Rank-count equality alone never proves layout compatibility. Equal-degree
boundaries with incompatible axes, ownership, padding, or layouts still require
an explicit reshard/layout transformation. If the adapter does not certify the
transition, the placement is infeasible before Selection.

## Data-Driven Activation

- An unsplit pipeline stage activates when its local fragment is ready and all
  direct authenticated predecessor data is ready.
- A tensor stage activates its collective epoch when every selected rank is
  locally ready and the group's direct authenticated input is ready.
- A downstream stage activates only after its normal dependency or redistribution
  edge reaches its complete validated state.
- No stage waits for unrelated stages/Providers, no global model-ready cover is
  required, and no second execution-start command exists.

## Complete-Output and Oracle Contract

Pipeline-only `[1,1,...,1]`, uniform tensor, and heterogeneous vectors such as
`[1,2,1]` and `[2,1,2]` must return the same complete application output as a
frozen unsplit oracle under a predeclared backend/dtype tolerance.

Trace invariants:

- stage rank count equals `M_i`;
- stages with `M_i = 1` have no tensor collective;
- collectives occur only in their owning stage/group/epoch;
- redistribution occurs only at declared boundaries and exactly once under its
  completion semantics;
- no downstream role consumes partial, duplicate, wrong-attempt, or stale-epoch
  output;
- the terminal writer produces one authenticated complete Response or one
  classified terminal failure.

## Failure Contract

Reject before Selection:

- `M_i < 1`, incomplete or duplicate rank cover;
- illegal tensor axis/layout/padding or uncovered tensor;
- missing/unsupported redistribution;
- incompatible backend/device/collective topology;
- per-device resource-envelope overflow;
- cyclic pipeline/redistribution graph;
- stale offer/profile/snapshot or unoffered device handle.

Fail the exact active group/attempt:

- rank/device loss or communicator epoch mismatch;
- collective ordering/timeout/integrity failure;
- corrupt dependency/redistribution data;
- local preparation/load failure;
- cancellation or hard/no-progress deadline.

Transport-specific negatives additionally include wrong capability/peer/key,
oversized manifest, missing or conflicting segment, operation reordering,
stale/replayed epoch, completion before full bitmap, and cancellation followed by
late Data. Each must produce zero partial downstream output.

Every failure retains public request ID, attempt/plan/group epoch, lifecycle
class, and last verified progress checkpoint.
