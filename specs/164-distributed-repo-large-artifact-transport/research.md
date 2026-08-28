# Research: DistributedRepo Large-Artifact Transport

**Feature**: [spec.md](spec.md)

**Date**: 2026-07-29

**Status**: Design-input audit complete; implementation not started

## Executive Decision

Create Spec 164 as an independent generic NDNSF-DistributedRepo feature.
Spec 162 remains a consumer and preserves its current repository path as the
legacy performance baseline. Spec 163 remains responsible for distributed-
inference planning and must not absorb repository transport or persistence.

The proposed optimization is sound after one security correction:

```text
publisher-authenticated bounded root manifest
  → digest-authenticated manifest pages
  → digest-authenticated chunks and Data
```

A shared HMAC key is not required for public artifact verification. Bulk Data
should be verified through collision-resistant digests transitively bound to
the authenticated root. HMAC remains optional only for a closed-domain session
or upload-authentication policy.

The first persistence design to plan is:

```text
streaming filesystem content-addressed payload store
  + transactional embedded metadata store
```

Replacing the current database engine is not yet justified. The first
bottleneck to remove is the current per-wire-packet record and query model.

## Decision 1: Separate Spec 164

### Decision

Place large immutable artifact transfer, manifest security, persistence
separation, API usability, resume, and throughput validation in
`specs/164-distributed-repo-large-artifact-transport`.

### Rationale

- The capability serves models, datasets, checkpoints, video, and arbitrary
  immutable artifacts.
- NDNSF-DI should consume it without owning its wire, persistence, or security
  semantics.
- Spec 162 needs a stable generic repository dependency before its large-model
  campaign can make meaningful publication-time claims.
- Spec 163 concerns ACK-driven model partition and role planning, not artifact
  storage or transfer.

### Rejected Alternatives

- **Fold into Spec 162**: rejected because it would make a generic repository
  redesign appear to be a Qwen experiment detail.
- **Fold into Spec 163**: rejected because it would move repository data-plane
  logic into inference planning.
- **Tune the existing large-object script only**: rejected because larger
  chunks and timeouts do not remove per-packet verification, serialization,
  metadata cardinality, or lifecycle ambiguity.

## Decision 2: Signed Root Manifest, Digest-Authenticated Bulk Data

### Decision

The root manifest is authenticated under the NDNSF trust policy with a
negotiated asymmetric signature. Manifest pages and bulk content are
authenticated with collision-resistant digests bound transitively to that
root. The complete artifact digest is checked before activation.

### Why HMAC Is Not the Public Default

HMAC-SHA256 authenticates with a shared secret. Any consumer or cache that can
verify the HMAC can also create a different valid HMAC. This does not provide
public publisher provenance and introduces key-distribution and cache-sharing
problems.

Once the trusted root authenticates the expected content digests, HMAC does
not add artifact-origin assurance to each Data packet. A plain digest
comparison is cheaper, cache-compatible, and sufficient for content integrity
under the signed-root composition.

HMAC may still be useful where all participants deliberately share a closed-
domain secret, for example:

- authorizing one upload lease;
- authenticating a private session between already selected peers;
- protecting a non-public operational channel.

Such use must be separately negotiated and must not become the immutable
artifact identity or the only proof of publisher provenance.

### Signature Algorithm

Do not hard-code RSA. The wire contract names the algorithm and the trust
policy decides which algorithms are acceptable. RSA and ECDSA are already
relevant to the local stack; other algorithms require an explicit capability
and interoperability proof.

Because only a bounded root trust structure is asymmetrically authenticated,
signature verification should not dominate multi-gigabyte transfer. This is a
testable hypothesis, not an assumed result.

### Primary References

- [NDN Packet Signature Specification](https://docs.named-data.net/NDN-packet-spec/current/signature.html)
  defines DigestSha256, RSA, ECDSA, HMAC-SHA256, and Ed25519 signature types and
  their security meanings.
- [NDN Technical Report: Manifest Embedding](https://named-data.net/publications/techreports/ndn-tr-25-manifest-embedding/)
  describes authenticating a manifest with public-key cryptography and using
  digest-bound names for efficient Data verification.
- [NDN ContentType registry](https://redmine.named-data.net/projects/ndn-tlv/wiki/ContentType)
  reserves ContentType value 4 for Manifest.

## Decision 3: Hierarchical Manifest, Not One Root Entry per Data Packet

### Decision

Use a bounded hierarchy of root manifest, content-addressed manifest pages, and
chunk digests. Derive Data names from an authenticated naming template and
chunk/segment coordinates instead of repeating every full Data name.

### Scale Analysis

The observed Qwen stage preparation produced approximately 13.45 million
roughly 4 KiB Data packets for about 53.79 GB. Storing only one 32-byte digest
per packet would already require approximately 430 MB before names and TLV
overhead. Repeating full names can push one flat manifest far beyond that.

A monolithic manifest would therefore:

- require its own large segmented transfer and verification state;
- impose a large parse and allocation event;
- make random resume and partial verification expensive;
- create a denial-of-service target;
- duplicate predictable name components millions of times.

### Proposed Logical Shape

```text
RootManifest
  artifact identity
  logical naming scope
  total size
  packet and chunk geometry
  naming template
  digest and signature algorithms
  root of manifest hierarchy
  publisher and policy context

ManifestPage
  bounded coordinate range
  child-page or chunk digests

Chunk
  bounded artifact byte range
  chunk digest
  derived Data names/segments
```

The plan must select sizes through a compatibility and performance study. The
spec only requires bounded root/page sizes and no root growth proportional to
Data-packet count.

## Decision 4: One NDNSF Control Collaboration, Streaming NDN Data Plane

### Decision

Use the existing NDNSF collaboration/security path once to discover and select
repository replicas and establish a transfer lease. Transfer bytes through a
pipelined segmented NDN producer/fetcher. Commit and replica receipts close the
repository lifecycle.

### Required Separation

```text
NDNSF control plane
  discover → authorize → select replicas → reserve → lease

NDN data plane
  express pipelined Interests → receive out of order
  → validate → write ranges → retransmit missing ranges

NDNSF lifecycle close
  commit → replica receipts → catalog activation
```

No NDNSF service request is issued per Data packet or application chunk.
Control-operation count must remain independent of artifact size.

This repository commit means atomic storage publication. It is unrelated to
the rejected DI `PreparationCommit` concept.

### Existing Source Evidence

- `NetworkDistributedRepoClient` already distinguishes normal and Targeted
  control calls but serializes them through a single control executor:
  `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py:5233`.
- `_packet_to_request` serializes each signed wire packet and digest into a
  control request representation:
  `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py:5517`.
- `put_file()` delegates through `put()` to `store_object()`:
  `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py:7768`.
- The local stack selects an EC key before falling back to RSA:
  `ndn-service-framework/common.cpp:73`.
- ndn-cxx already provides a congestion-controlled segmented fetcher:
  [SegmentFetcher](https://docs.named-data.net/ndn-cxx/master/doxygen/classndn_1_1SegmentFetcher.html).

## Decision 5: Separate Payload and Metadata Persistence

### Decision

Define two authoritative persistence contracts:

```text
PayloadStore
  begin or resume an exact artifact identity
  write and read bounded ranges
  verify and atomically finalize
  abort and reclaim temporary state
  test existence and committed identity

MetadataStore
  manifests and capabilities
  sessions, leases, and verified progress
  lifecycle and catalog state
  replica receipts and policy epochs
  garbage-collection ownership
```

The initial plan should evaluate filesystem content-addressed payloads plus an
embedded transactional metadata store. Metadata may track chunks or compact
range maps, but the scalable format must not require one durable row per 4 KiB
wire Data packet.

### Why SQLite Is Not Yet Rejected

SQLite can serve a single application-level writer well when transactions are
batched, payload blobs are not fragmented into millions of tiny records, and
metadata access is indexed. WAL permits readers to proceed with a writer, but
one database still has one writer at a time.

Relevant SQLite guidance:

- [Write-Ahead Logging](https://www.sqlite.org/wal.html) documents reader/writer
  concurrency and the single-writer boundary.
- [Appropriate Uses For SQLite](https://www.sqlite.org/whentouse.html) recommends
  a client/server engine when many independent writers contend on the same
  database.
- [35% Faster Than The Filesystem](https://www.sqlite.org/fasterthanfs.html)
  shows that small blobs can perform well while larger blobs increasingly
  favor direct files, and that transaction batching materially changes
  results.

The current problem is therefore not proved to be “SQLite is slow.” It is:

- payload bytes stored as many exact wire-packet rows;
- per-packet selects and durable work;
- repeated scans and byte-accounting work;
- a deployed Python persistence path separate from the C++ backend contract.

### Why RocksDB Is Not the Immediate Answer

Moving the same millions-of-packet-row model into RocksDB would preserve the
high key count and add LSM compaction behavior. RocksDB explicitly exposes
read, write, and space amplification and can stall writes when memtable or
compaction limits are reached:

- [RocksDB Tuning Guide](https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide)
- [RocksDB Write Stalls](https://github.com/facebook/rocksdb/wiki/Write-Stalls)

Add a different metadata engine only if a matched benchmark demonstrates that
the corrected metadata workload is itself the limiting component.

### Backend Authority Issue

The C++ API already declares a `RepoStoreBackend` abstraction in
`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp:255`, while
the deployed Python repository path directly owns SQLite around
`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py:1372`.

The plan must choose one authoritative backend contract and route the deployed
runtime through it. Maintaining two persistence authorities would make crash,
migration, and compatibility behavior impossible to state rigorously.

## Decision 6: API Layers

### Simple Synchronous Surface

The common path should conceptually support:

```python
ref = repo.publish_file(
    path,
    name="/models/qwen/stage-0",
    expected_sha256=digest,
    replicas=1,
    verification="signed-manifest",
    resume=True,
    on_progress=progress,
)

repo.fetch_file(ref, destination, resume=True, verify=True)
```

This is a design sketch, not a frozen function signature.

### Asynchronous Surface

Applications must be able to await publication and retrieval, receive bounded
progress updates, cancel, and resume without managing framework threads or
private client fields.

### Advanced Session Surface

An advanced interface exposes begin, upload, status, commit, and abort for one
immutable artifact identity. It must document:

- idempotency key scope;
- lease and timeout behavior;
- cancellation and retry;
- replica receipts and achieved durability;
- resume identity checks;
- progress units and monotonicity;
- atomic destination visibility.

The application must not access `_client.control_mode` or comparable private
state to select normal behavior.

## Decision 7: Lifecycle and Failure Semantics

### Required State Model

```text
ABSENT
  → RESERVED
  → RECEIVING
  → VERIFIED
  → COMMITTED
  → ACTIVE

RESERVED / RECEIVING / VERIFIED
  → FAILED or EXPIRED
```

`COMMITTED` means the payload is durably finalized by a replica.
`ACTIVE` means policy-required receipts and catalog publication make the
artifact discoverable. Planning must decide whether a single-replica request
can combine these transitions while retaining their distinct semantics.

### Safety Invariants

- No partial or unverified artifact is discoverable.
- No resume combines different content identities, manifest roots, or policy
  epochs.
- A reported replica exists only when a retained authenticated receipt exists.
- A crash between filesystem finalization and metadata activation recovers to
  one unambiguous valid state.
- Garbage collection cannot delete active, committed, leased, or currently
  finalized content.
- Deduplicated bytes do not transfer one publisher's provenance to another.

## Decision 8: Performance Evidence

### Three Ceilings

1. Network ceiling: capacity of the matched host or namespace path.
2. Raw NDN ceiling: segmented NDN transfer without repository control,
   persistence, replication, or manifest traversal.
3. Repository goodput: logical artifact bytes divided by completion through
   required commit and activation.

Comparing only against nominal interface speed is insufficient.

### Preliminary Gates

- Digest-only goodput at least 85% of matched raw NDN goodput for 64 MiB and
  larger artifacts.
- Signed-manifest goodput no more than 10% below digest-only.
- Control calls independent of artifact bytes and packet count.
- Metadata count independent of 4 KiB Data-packet count.
- Peak memory independent of artifact size at a fixed in-flight window.
- Cold single-replica read amplification at most 1.20x logical bytes.
- Cold single-replica write amplification at most 1.50x logical bytes.
- No integrity, provenance, atomic-visibility, or recovery failure.

These are frozen engineering acceptance gates. At least five repetitions are
required per admissible cell. Paper-level inference needs a pilot-derived
sample size fixed before its formal campaign. See
[experiment-plan.md](experiment-plan.md).

## Compatibility and Migration

### Formats

- `exact-packet-v1`: retains original wire-packet behavior for legacy and
  provenance-preservation use cases.
- `artifact-manifest-v2`: scalable default for large immutable artifacts.

The final wire names are a planning output. The version distinction and
prohibition on silent downgrade are specification requirements.

### Migration

- Do not rewrite existing objects in place.
- New publishers capability-negotiate the desired format.
- Mixed replica sets report achieved capability and durability.
- Indexing and garbage collection preserve format identity.
- Rollback disables new publication without reinterpreting or deleting already
  committed objects.

## Academic Audit Verdict

| Question | Verdict | Reason |
|---|---|---|
| Is a separate Spec necessary? | PASS | Generic repository transport has a distinct owner, API, security contract, lifecycle, and evidence gate. |
| Is one signed manifest plus cheap Data verification sound? | CONDITIONAL PASS | Sound when every accepted byte is transitively digest-bound to a trusted, bounded root. |
| Should each public Data packet use a shared HMAC? | REJECT AS DEFAULT | Shared verifiers can forge; it adds no provenance after signed-root digest binding. |
| Should the root list every packet name and authenticator? | REJECT | It grows to hundreds of megabytes or more for observed artifact sizes. |
| Is SQLite proven inadequate? | NOT PROVEN | Current per-packet schema and access pattern must be removed before the engine is judged. |
| Should RocksDB replace SQLite now? | BLOCK | No matched evidence; the same record model would retain cardinality and add compaction risks. |
| Is implementation ready? | BLOCK UNTIL PLAN | Wire schema, limits, backend authority, migration, threat model, and matched benchmark harness require planning and audit. |

## Planning Questions Already Resolved

- **Public HMAC requirement**: no; signed root plus digest hierarchy is the
  default.
- **Asymmetric algorithm**: capability and trust-policy driven; not RSA-only.
- **Manifest organization**: bounded hierarchy, not a monolithic packet list.
- **Initial persistence direction**: content-addressed files plus transactional
  metadata, subject to benchmark.
- **Database replacement**: evidence-driven, not assumed.
- **Control carrier**: existing NDNSF collaboration and security.
- **Bulk carrier**: pipelined segmented NDN data.
- **Validation environment**: MiniNDN first; TigerCluster only after local gate
  or explicit request.
- **Legacy behavior**: retained as an explicit format and performance baseline.

## Remaining Planning Work

The plan must freeze:

1. root and page TLV schemas, name derivation, version negotiation, and limits;
2. trust-schema validation, revocation semantics, and algorithm capability
   matrix;
3. chunk/page/segment geometry selection method;
4. authoritative payload and metadata backend interfaces;
5. crash-consistent finalize/activate transaction and garbage collection;
6. sync, async, and advanced session API contracts;
7. migration and rollback procedures;
8. MiniNDN topology, raw NDN subject, instrumentation, campaign schema, and
   admissibility rules;
9. threat model and focused negative tests;
10. task decomposition into cohesive behavioral outcomes.

