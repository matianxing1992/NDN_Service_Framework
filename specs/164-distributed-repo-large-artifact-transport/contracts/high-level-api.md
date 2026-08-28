# Contract: High-Level Artifact API

## Design Status

Normative behavioral contract; concrete Python/C++ spelling may change only
through a documented contract revision.

## Simple Publication

```python
published = repo.publish_file(
    path,
    name="/models/qwen/stage-0",
    expected_sha256=digest,
    replicas=1,
    verification="signed-manifest",
    resume=True,
    on_progress=progress,
)
ref = published.reference
```

The operation:

1. derives or validates the immutable identity;
2. negotiates `artifact-manifest-v2`;
3. selects authorized capable replicas through NDNSF collaboration;
4. commits exact Provider assignments after ACK_CLOSED; ACKs do not reserve
   capacity;
5. selected Providers enqueue bounded store tasks and serve/fetch the artifact
   through the segmented data plane;
6. waits for the requested commit receipts or returns achieved durability;
7. returns one stable `ArtifactPublishResult` whose `reference` is the immutable
   `ArtifactReference` and whose achieved durability equals its distinct
   authenticated receipt identifiers.

It does not expose packet batching, replica-specific control calls, or private
control-mode state.

## Simple Retrieval

```python
fetched = repo.fetch_file(
    ref,
    destination,
    resume=True,
    verify=True,
    on_progress=progress,
)
```

The destination becomes visible atomically only after required trust and
content verification. An existing matching destination may return a verified
deduplication result. A conflicting destination is never overwritten without
an explicit replacement policy.

## Asynchronous API

```python
published = await repo.publish_file_async(...)
fetched = await repo.fetch_file_async(...)
```

Cancellation requests stop new network work, preserve only policy-valid
resumable progress, and return a stable cancellation result. Callbacks or
iterators are bounded and cannot block the transfer engine indefinitely.

## Advanced Session API

```python
session = repo.begin_upload(descriptor, on_progress=progress)
session.upload_file(path)
status = session.status()
published = session.commit()
session.abort()
```

Equivalent retrieval sessions expose begin, transfer, status, commit/finalize,
and abort. Repeated begin/commit/abort with the same identity is deterministic.

`ArtifactDescriptor` contains the idempotency key. The operation identifier is
derived from direction, key, and the complete immutable artifact identity, so a
key cannot alias different bytes, roots, or policy epochs.

## Public Control Selection

```python
control = ArtifactControlOptions(
    mode=ArtifactControlMode.TARGETED,
    targeted_provider="/repo/selected",
)
published = repo.publish_file(..., control=control)
```

The default is generic NDNSF collaboration. Targeted selection requires one
explicit provider. Applications never mutate `_client.control_mode`, select
wire control operations, or send replica-specific assignment messages.

## Artifact Descriptor

Required fields:

- logical name;
- expected full content digest and algorithm;
- exact size;
- format preference;
- publisher/trust context;
- requested replicas;
- idempotency key.

Optional fields:

- policy epoch;
- expiry;
- chunk/packet geometry preference within advertised limits;
- progress observer;
- timeout and cancellation token;
- resume policy;
- metadata that is explicitly covered by the root manifest.

## Progress Contract

Progress events contain:

- operation and artifact identity;
- phase;
- logical bytes received/verified/committed;
- total logical bytes;
- selected and committed replicas;
- retransmitted bytes when available;
- monotonic sequence and timestamp.

`receivedBytes` is not presented as `verifiedBytes`, and `verifiedBytes` is not
presented as replicated/active durability.

## Result Contract

`ArtifactPublishResult` returns:

- ArtifactReference;
- requested and achieved replica counts;
- ReplicaReceipts;
- total and phase durations;
- deduplication/resume outcome;
- stable operation identity.

`ArtifactFetchResult` returns:

- verified ArtifactReference;
- destination;
- bytes reused and transferred;
- source replica(s);
- total and phase durations.

## Stable Error Categories

- `INVALID_ARGUMENT`
- `UNSUPPORTED_CAPABILITY`
- `AUTHORIZATION_FAILED`
- `TRUST_VALIDATION_FAILED`
- `MANIFEST_INVALID`
- `CONTENT_DIGEST_MISMATCH`
- `LEASE_EXPIRED`
- `CAPACITY_UNAVAILABLE`
- `TRANSFER_TIMEOUT`
- `CANCELLED`
- `REPLICA_COMMIT_FAILED`
- `DURABILITY_NOT_ACHIEVED`
- `DESTINATION_CONFLICT`
- `RECOVERY_REQUIRED`
- `INTERNAL_ERROR`

Errors include operation/artifact identity and achieved durable state. They do
not expose secrets, raw keys, or unbounded peer-controlled diagnostic text.
Unexpected backend exceptions are reduced to a bounded generic
`INTERNAL_ERROR`; raw backend exception text is not copied into the public
diagnostic.

## Compatibility

Legacy exact-packet calls remain available under an explicit format/method.
Large-object APIs default to `artifact-manifest-v2` only after capability
negotiation. Silent fallback to weaker verification or different format is
prohibited.

The exact advertisement fields, rejection reasons, and distinct-replica
durability rule are defined in
[`capability-negotiation.md`](capability-negotiation.md). Applications that
intentionally preserve application-signed Data wires use
`DistributedRepo.exact_packets`; v2 `publish_file(...)` never selects that
backend as a fallback.

Schema startup, additive roll-forward, read-only rollback, complete format
identity, and operator-visible diagnostics are defined in
[`migration-rollback.md`](migration-rollback.md). A read-only rollback node
withdraws v2 write capability, so application calls fail during capability
negotiation rather than after payload transfer.
