# Data Model: Unified Named UAV Video

## CanonicalVideoPacketRecord

Immutable observation of a packet created by the authoritative LiveStream publisher.

Fields:

- stream/session and Mapping version
- packet kind: `Source`, `Mapping`, or optional `Repair`
- source cursor or Mapping cursor range
- original semantic Data name
- verified Provider identity expected by the stream definition
- complete signed Data wire
- SHA-256 wire digest
- materialization timestamp from one monotonic clock domain

Rules:

- One semantic source name binds once to one wire digest within a session.
- Source and Mapping records are retention-authoritative; Repair records are optional transport evidence and never substitute for a source.
- The record contains no plaintext H.264, content key, nonce salt, or Repo policy.

## RetentionSession

APP-owned lifecycle joining one LiveStream publication session to one bounded durable writer.

States:

```text
Disabled -> Starting -> Active -> Finalizing -> Complete
                    \-> Degraded -> Finalizing
                    \-> Failed
```

Fields:

- recording ID and canonical stream/session identity
- publisher and camera identity
- queue capacity/high-water mark
- staged and durable cursor ranges
- retained Mapping blocks and source packet digests
- explicit gap ranges and reasons
- current manifest version and checkpoint
- start/end time and completion state
- key-authorization reference, never key bytes
- decoder-safe starting cursor and atomic attachment checkpoint

Rules:

- Staged data is never advertised as durable.
- Checkpoints advance monotonically across complete contiguous retained ranges.
- Failure may add a gap or end recording, but cannot rename/rewrite a canonical packet.

## RecordingManifest

Provider-signed durable index for replay.

Fields:

- manifest name/version and recording ID
- canonical LiveStream descriptor snapshot
- Provider, service, session/key epoch, encoding
- first/last committed cursor and time
- Mapping checkpoint and retained Mapping packet digests
- canonical source packet names/digests or segmented catalog reference
- gaps and completion state
- recipient-independent content-key authorization reference
- Provider signing-certificate name/digest, retained chain, trust-policy version, and authenticated session/capture interval

Rules:

- No media bytes or plaintext key material.
- Every referenced packet is durable before checkpoint visibility.
- Conflicting manifest versions, rollback, gaps represented as complete, or wrong signer/session fail closed.

## ContentKeyGrant

Permission- and recipient-bound authorization for the canonical session content key.

Bindings:

- recipient identity/certificate
- Provider and service/permission
- stream/session and key epoch
- cipher/contract version
- encrypted key material and integrity proof

Rules:

- A live or historical grant may differ in recipient, permission, and authorized epoch set, but each granted epoch resolves to the same key that protected those canonical packets.
- A grant never changes packet name, wire, ciphertext, or signature.
- Revocation withholds or rotates future epochs; it cannot invalidate an epoch key already disclosed legitimately.

## RetentionGap

Explicit non-overlapping cursor/time range with reason such as queue overflow, storage failure, missing Mapping, shutdown timeout, or integrity rejection.

Rules:

- A gap cannot later be silently declared complete.
- Replay may stop or skip according to operator policy, but must surface the gap before decoder continuation.

## PerformanceTimeline

Bounded sampled evidence for one canonical item across its owning clock domains.

Fields:

- stable correlation key: stream ID, session epoch, cursor, frame ID, segment index
- stage/event kind and owning process/clock-domain ID
- monotonic timestamp and optional authenticated acquisition timestamp
- wall-clock offset estimate and uncertainty bound when cross-node comparison is enabled
- sampler version/rate and selected/not-selected decision
- queue depth/high-water, byte count and outcome at the event boundary
- missing/drop reason rather than a fabricated timestamp

Rules:

- The same stable sampler decision follows a cursor through every stage.
- A stage records only the event it owns; it does not estimate another process's time.
- Local durations subtract monotonic timestamps from the same clock domain.
- Cross-node one-way durations are unavailable unless offset uncertainty is recorded and acceptable.
- `encoded-output-ready` begins when the APP reads encoded H.264 output; it is not camera capture.
- Trace queues and retained evidence are bounded and expose every dropped sample.

## Relationships

```text
one LiveStream session
  -> many CanonicalVideoPacketRecords
  -> zero or one active RetentionSession
       -> versioned RecordingManifests
       -> zero or more RetentionGaps
       -> many opaque Repo objects containing exact signed wires
  -> many ContentKeyGrants for authorized live/history users
  -> zero or more sampled PerformanceTimelines correlated to canonical cursors
```
