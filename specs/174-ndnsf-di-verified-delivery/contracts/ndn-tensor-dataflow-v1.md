# NDN Tensor Dataflow Contract v1

## Scope

Defines how existing NDNSF-DI dependency I/O, tensor codecs, role workers, and collective runtime exchange every cross-Provider activation, partial, and collective result. Local in-process queues may connect components inside one Provider process, but cannot represent a cross-Provider edge in acceptance evidence.

## Transport Rule

For different Providers:

```text
consumer sends Interest for expected manifest
producer returns signed manifest Data
consumer verifies full manifest contract
consumer sends Interests for exact segment names
producer/cache returns signed segment Data
consumer verifies, deduplicates, reconstructs, and emits one ready event
```

TCP/RPC push, shared files, a shared object reference, hidden collective sockets, or opaque cross-Provider NCCL are forbidden substitutes. NFD may use link-layer transports internally; the application dependency semantics remain Interest/Data and exact NDN names.

## Namespace Ownership

- canonical persistent model objects: distributed repository namespace;
- request-scoped activation/partial/result objects: producing Provider namespace;
- final NDNSF response: normal Provider response namespace.

The producer signs the Data it owns. The consumer does not rename or republish an object as if it were the producer.

## Deterministic Logical Name

The exact TLV encoding is mapped to current production codecs during implementation, but the canonical logical components are mandatory:

```text
/<producer>/NDNSF/DI/TENSOR
  /<requester-component>
  /<requestId>
  /ATTEMPT/<attemptId>
  /PLAN/<planDigest>
  /GEN/<generation>
  /GROUP/<groupId-or-none>
  /EPOCH/<groupEpoch>
  /OP/<operationId>
  /ROUND/<round>
  /ROLE/<producerRoleId>
  /RANK/<producerRank-or-none>
  /TENSOR/<tensorId>
  /MICROBATCH/<microbatchId>
  /DIGEST/<contentDigest>
  /MANIFEST
```

Segment Data replaces the final `MANIFEST` component with `/SEGMENT/<segmentNo>`. If current wire helpers use a different component order or escaping, one canonical schema must be frozen and tested; aliases are not accepted within one plan.

## Manifest Requirements

The signed manifest contains:

- full logical object identity and producer;
- request, attempt, plan, generation, group/epoch, operation/round, role/rank, tensor, and microbatch bindings;
- dtype, shape, layout, canonical byte order, total byte length;
- segment count and size bounds;
- exact ordered segment names and per-segment digests;
- complete content digest and manifest digest;
- freshness/expiry and signature metadata.

Consumers compare expected full values. Size, suffix, rank alone, or payload digest without authority identity is insufficient.

## Publication Rules

1. The producing role computes one immutable object for one sealed output identity.
2. Canonical bytes and complete digest are produced before the final manifest is published.
3. Segments are immutable and available under their declared exact names for the bounded fetch window.
4. Repeated publication of identical name/content is idempotent; same name with different bytes is a fatal conflict.
5. Publication does not itself mark a consumer dependency ready.

## Fetch And Reconstruction Rules

1. Request/verify manifest before accepting segments.
2. Express Interests only for declared exact segment names.
3. Enforce configured Interest lifetime, per-segment retry bound, no-progress deadline, hard edge deadline, and request deadline.
4. Accept valid segments in any order and deduplicate exact duplicates.
5. Reject conflicting duplicates, undeclared segments, wrong signatures, stale attempts/plans/epochs, wrong layout, oversize content, and digest mismatch.
6. Reconstruct in manifest order, verify total length and complete digest, and emit object-ready exactly once.
7. After cancellation/fencing, late Data cannot make the edge ready.

## Retry Semantics

- Transport retry re-expresses the same Interest name.
- It cannot change Provider, role, rank, plan, epoch, tensor identity, or expected digest.
- It cannot trigger duplicate producer execution.
- Exhaustion produces a bounded dependency failure containing the first missing exact name.
- Provider replacement or rank membership change is a replan with new authority, not transport retry.

## Pipeline Semantics

Each downstream role declares its exact upstream tensor manifests. It becomes runnable after all required objects are ready and its local authorization/assembly is complete. Backpressure is bounded by declared in-flight object/byte limits; dropping an object silently is forbidden.

## Tensor Group Semantics

Each group seals rank membership, operation sequence, rounds, expected partials, and result owner. For every operation/round:

- each rank knows which exact objects it produces;
- each consumer rank knows which exact manifests it requests;
- computation such as reduction/concatenation is owned by an explicit rank role or merge role;
- the next round cannot consume an incomplete prior result;
- missing rank/partial fails the group epoch; world size does not shrink;
- group completion yields one planned stage result, not multiple accepted application responses.

## Hybrid Semantics

Pipeline and tensor edges share the same manifest/segment machinery and scheduler. A pipeline role can feed a tensor group, and the group's explicit result owner can feed another pipeline role. Boundary tensors retain the same request/attempt/plan identity.

## Packet-Fault Matrix

| Fault | Expected result |
|---|---|
| one segment lost then recovered | same-name retry; one ready object |
| segments reordered | exact reconstruction; one ready object |
| exact duplicate | ignored after validation |
| conflicting duplicate | fatal object rejection |
| corrupted payload | segment/content digest rejection |
| stale attempt/plan/epoch | authority rejection |
| wrong producer/rank/tensor/layout | contract rejection |
| replay after cancellation | fenced, no readiness |
| permanently missing segment/peer | bounded timeout with first missing name |
| consumer restart | resume only if active authority and state contract allow; otherwise new attempt |

## Evidence

For each planned edge, record bounded hashes/identities for manifest publish, manifest Interest/Data, segment Interest/Data counts, retries, ready/failure event, and first missing/invalid exact name. Do not record plaintext tensor bytes.
