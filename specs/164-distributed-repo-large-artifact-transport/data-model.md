# Data Model: DistributedRepo Large-Artifact Transport

## ArtifactReference

Stable reference returned to applications.

| Field | Meaning | Validation |
|---|---|---|
| `logicalName` | Human-readable application name | Canonical NDN name within authorized publisher scope |
| `contentDigest` | Immutable full-artifact identity | Supported collision-resistant algorithm and exact encoded length |
| `sizeBytes` | Exact reconstructed length | Non-negative and within policy limit |
| `formatVersion` | `exact-packet-v1` or `artifact-manifest-v2` | Must be advertised by selected replicas and consumer |
| `rootManifestName` | Name used to retrieve the authenticated root | Bound to logical name/version/content identity |
| `publisherIdentity` | Provenance identity | Must satisfy configured trust schema |
| `policyEpoch` | Policy/revocation evaluation context | Exact match during resume and explicit acceptance during retrieval |

Two references are byte-identical only when their content digest and size
match. Equal bytes do not imply equal publisher provenance or logical-name
authorization.

## RootManifest

Publisher-authenticated root of the artifact verification graph.

| Field | Meaning |
|---|---|
| `manifestVersion` | Versioned schema identifier |
| `artifact` | Bound ArtifactReference fields |
| `packetGeometry` | Payload limit and final-segment rules |
| `chunkGeometry` | Chunk coordinate and length rules |
| `nameTemplate` | Deterministic page/chunk/Data name derivation |
| `digestAlgorithm` | Digest used for pages, chunks, and full artifact |
| `signatureAlgorithm` | Root authentication algorithm |
| `manifestRoot` | Digest or reference for the first bounded page/tree node |
| `publisherKeyLocator` | Key/certificate lookup context |
| `createdAt` / `expiresAt` | Manifest validity information |
| `policyEpoch` | Trust-policy evaluation epoch |
| `criticalExtensions` | Versioned fields that unsupported readers must reject |

Validation occurs before fetching unbounded descendants. Encoded size,
geometry, algorithms, naming scope, validity, trust chain, and extension
support are policy-bounded.

## ManifestPage

Bounded content-addressed node authenticating child pages or chunks.

| Field | Meaning |
|---|---|
| `pageVersion` | Page schema version |
| `coordinateRange` | Non-overlapping artifact range covered |
| `children` | Ordered child digest, coordinate, type, and length entries |
| `pageDigest` | Content digest referenced by its parent |

Pages must be acyclic, ordered, non-overlapping, within the root's total size,
and bounded in encoded size, entry count, and depth.

## ArtifactChunk

Resumable unit between manifest pages and Data segments.

| Field | Meaning |
|---|---|
| `chunkIndex` | Deterministic ordinal |
| `offsetBytes` | Exact start within artifact |
| `lengthBytes` | Exact logical length |
| `chunkDigest` | Digest transitively bound by a ManifestPage |
| `segmentRange` | Deterministically derived Data segment coordinates |

Chunks cover the artifact exactly once with no gaps or overlap. The final chunk
may be shorter than the nominal chunk size.

## TransferSession

Idempotent publication or retrieval attempt for one exact ArtifactReference.

| Field | Meaning |
|---|---|
| `operationId` | Stable caller-visible idempotency identity |
| `direction` | `PUBLISH` or `FETCH` |
| `artifact` | Exact immutable reference |
| `selectedReplicas` | Authorized repository identities |
| `requestedReplicas` | Requested durability |
| `leaseId` / `expiresAt` | Transfer authority and bound |
| `state` | Lifecycle state |
| `verifiedProgress` | Compact verified chunk/range state |
| `lastError` | Stable error category and diagnostic |
| `createdAt` / `updatedAt` | Recovery and observability timestamps |

Resume requires the same operation/artifact identity, compatible policy epoch,
and valid internal transfer-session ownership. It never merges different
manifests.

## StoreOffer and Internal TransferSessionIdentity

`StoreOffer` is ACK metadata containing bounded queue/load/capacity
observations. It is advisory and creates no reservation, lock, pin, or right to
transfer. `StoreAssignment` is the Selection-bound exact task identity.

An internal `TransferSessionIdentity` binds the selected `taskId`, repository
identity, artifact identity, allowed naming scope, accepted format/algorithms,
replay identity, and repository authentication after task execution starts. It
labels partial-progress and recovery state; it is neither a resource lease nor
a lock, reserves no declared bytes, and is never issued by ACK. It cannot
authorize a different artifact or be replayed after commit, abort, or cleanup.

## VerifiedProgressMap

Compact record of fully verified chunks or non-overlapping ranges. Progress is
monotonic for one immutable identity. Receiving bytes is not verified progress;
a range becomes resumable only after its required digest check and durable
write boundary.

## ReplicaReceipt

Repository-authenticated evidence of a durable commit.

| Field | Meaning |
|---|---|
| `repositoryIdentity` | Committing replica |
| `artifact` | Exact committed identity |
| `formatVersion` | Stored representation |
| `committedAt` | Commit time |
| `policyEpoch` | Policy applied |
| `storageGeneration` | Recovery/migration generation |
| `receiptId` | Replay-safe receipt identity |

Requested durability is satisfied only by distinct valid receipts retained in
metadata. A receipt is not proof that a different policy still regards the
publisher as trusted.

## CapabilityAdvertisement

Repository declaration containing supported formats, manifest/page versions,
signature and digest algorithms, size/depth limits, resume support,
replication/receipt support, and relevant policy epoch. Negotiation failure is
explicit; silent downgrade is prohibited.

## PayloadRecord

Backend-neutral payload state:

- artifact identity and size;
- temporary location or generation;
- verified length/chunk map reference;
- final content-addressed location;
- created/updated timestamps;
- exact selected `taskId` owner;
- finalization marker.

Payload bytes are not modeled as one metadata record per 4 KiB Data packet in
`artifact-manifest-v2`.

## MetadataRecord Set

Transactional metadata includes:

- artifact catalog entry;
- root/page manifest references;
- queued task and internal transfer-session ownership;
- verified progress;
- replica receipts;
- lifecycle journal/finalization intent;
- policy/revocation state;
- garbage-collection ownership and deadline.

## Lifecycle Transitions

| From | Event | To | Required invariant |
|---|---|---|---|
| `ABSENT` | Selected task is accepted | `QUEUED` | No bytes or capacity are reserved |
| `QUEUED` | Provider begins task execution | `RECEIVING` | Temporary payload remains hidden |
| `RECEIVING` | All chunks and full digest verified | `VERIFIED` | Exact size and identity match |
| `VERIFIED` | Atomic payload finalize and durable metadata intent | `COMMITTED` | Crash recovery can prove final bytes |
| `COMMITTED` | Required receipts and catalog transaction | `ACTIVE` | Discoverable identity is fully verified |
| `QUEUED`/`RECEIVING`/`VERIFIED` | Abort, expiry, or unrecoverable error | `FAILED` or `EXPIRED` | No active catalog entry |

Legacy `RESERVED` rows remain readable for migration and rollback, but the new
store-task path never creates them.

Illegal transitions are rejected and recorded. Recovery replays finalization
intent idempotently; it never guesses based only on file presence.

## Cross-Entity Invariants

1. Every active catalog entry resolves to one committed exact ArtifactReference.
2. Every reconstructed byte is transitively authenticated by its accepted root.
3. Logical-name authorization and byte deduplication remain separate.
4. Verified progress belongs to exactly one artifact, geometry, and policy
   context.
5. Replica count equals distinct retained valid receipts, not attempted writes.
6. Garbage collection cannot own an active, committed, or leased generation.
7. Format version is preserved through catalog, receipts, migration, and
   rollback.
