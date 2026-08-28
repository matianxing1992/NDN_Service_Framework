# Research Decisions: Unified Named UAV Video

## Decision 1: Retain the exact signed live packet

**Decision**: The recording stores byte-identical signed source Data wires and signed Mapping wires emitted by the authoritative LiveStream publisher.

**Rationale**: Current code encodes H.264 once but then creates recording-only raw chunks, names, encryption, and playback. Exact-wire retention produces one name, ciphertext, signature, cache identity, and validation path.

**Alternatives considered**:

- Store raw H.264 again: rejected because it preserves the current duplicate representation.
- Recreate Data during replay: rejected because re-signing changes packet identity and trust evidence.
- Treat Repo as another stream protocol: rejected because persistence and live scheduling have different ownership.

## Decision 2: Names-only Mapping first; nested canonical packet only by evidence

**Decision**: Mapping carries signed cursor-to-semantic-name bindings and checkpoints by default. First adapt and measure its lead. Only if that bounded design cannot meet the latency gate may a separately versioned mode carry the exact complete canonical signed source Data wire.

**Rationale**: When Mapping leads production by at least RTT plus jitter, exact payload Interests reach the Provider before production and steady-state production-to-delivery is approximately one-way. Inlining can reduce insufficient-lead latency. Embedding the complete inner signed Data wire preserves its semantic name and canonical identity, but requires independent inner/outer validation, explicit bindings, recursion/downgrade prevention, wire-cap control, and honest cache/bandwidth accounting. Smaller segments solve only the size symptom.

**Alternatives considered**:

- Embed complete signed Data in Mapping: retained only as a disabled, versioned candidate if names-only lead fails its measured gate; it must embed the canonical wire rather than create another payload.
- Embed payload bytes without inner Data: rejected because Mapping becomes the real payload name and semantic source names become metadata.
- Remove Mapping and use only formulaic payload names: rejected because the accepted design preserves original application names and supports variable semantic objects.

## Decision 3: Make Mapping lead measurable and adaptive

**Decision**: Size the bounded Mapping reservation horizon from measured RTT, production period, and jitter, and validate success through Provider-side Interest-before-production hits correlated by session/cursor.

**Rationale**: “Prefetch exists” is not sufficient. With simultaneous Mapping and payload availability, the path is about `3/2 RTT`; the `1/2 RTT` production-to-delivery goal requires the exact payload Interest to be pending first.

**Alternatives considered**:

- Fixed four-block horizon only: retained as a cap/default input, not accepted as proof of adequate time lead.
- Cross-host timestamp subtraction: rejected because clock synchronization is not a correctness assumption.

## Decision 4: Core exposes an app-neutral bounded published-packet feed

**Decision**: LiveStream places immutable packet records into a bounded feed after signing/materialization. The APP-owned worker drains the feed; Core never invokes APP or Repo code from the Face/I/O path.

**Rationale**: Only Core owns the exact signed wire. Reconstructing it in UAV would violate the one-packet invariant. Importing Repo into Core would violate ownership. A callback contract alone cannot prevent a slow APP callback from blocking network I/O, while a bounded pull feed makes the safety property enforceable.

**Alternatives considered**:

- UAV signs a second stored packet: rejected because it creates a second identity.
- Core writes Repo directly: rejected because a reusable stream must not own application persistence policy.

## Decision 5: Encrypt media once per packet epoch; grant epoch keys separately

**Decision**: One key epoch protects each canonical packet exactly once. Live and historical authorization return recipient-bound grants for the same epoch key through protected NDNSF responses; historical authorization can grant an allowed set of prior epochs. Manifests and Repo never contain plaintext keys.

**Rationale**: Different permissions do not require different media ciphertext. Epoch grants preserve one payload while binding service/permission, identity, session, and time range. Once a recipient learns an epoch key, revocation cannot erase it; rotation limits revocation to future packets honestly.

**Alternatives considered**:

- Separate live and recording ciphertext: rejected because it is the duplicate-data defect.
- Public recording key in manifest: rejected as a confidentiality failure.
- Per-consumer media encryption: rejected because it destroys cache sharing and immutable packet identity.

## Decision 6: Late retention starts from an atomic safe checkpoint

**Decision**: Attaching retention to an active publisher opens one bounded feed whose initial snapshot contains required signed Mapping/source records and whose future records begin without a race, then starts at the next Mapping-covered H.264 decoder-safe join.

**Rationale**: A feed opened after Mapping publication can otherwise miss the bindings needed to replay future source packets, and starting at an arbitrary delta frame creates an advertised but undecodable recording.

**Alternatives considered**:

- Require recording only at camera start: rejected because viewing and retention lifecycles are intended to remain independent.
- Backfill the entire live session: rejected because publisher retention is bounded and the operator asked to start recording now, not fabricate unavailable history.
- Start at the next arbitrary packet: rejected because H.264 playback may lack SPS/PPS/IDR reset material.

## Decision 7: Replay serves stored packets unchanged

**Decision**: A replay producer registers the archived Mapping and semantic names and serves stored packet wires without re-signing; the existing LiveStream consumer plus UAV callback performs the same validation/decryption/decode admission.

**Rationale**: A separate raw-chunk fetch/decrypt/reassembly/decoder path can drift from live security and naming rules.

**Alternatives considered**:

- Reassemble a temporary `.h264` file from recording chunks: rejected as duplicate playback logic.
- Bypass LiveStream validation for trusted local Repo: rejected because repository storage is not authority.

## Decision 8: Publication survives bounded retention failure

**Decision**: Repo work runs in an APP-owned bounded worker queue. Queue overflow or storage failure creates an explicit retention gap/degraded state but does not block the network I/O thread or silently terminate healthy live publication.

**Rationale**: Recording and viewing retain independent lifecycles even though they share data.

**Alternatives considered**:

- Synchronous storage before publication: rejected because storage latency would become live latency and could block I/O.
- Unbounded asynchronous queue: rejected because an outage could exhaust UAV memory.

## Decision 9: No automatic legacy recording migration

**Decision**: New writes use only canonical format. Experimental raw-chunk databases are detected and rejected with export/cleanup guidance.

**Rationale**: Old chunks cannot be transformed into the original historical signed live packet identity, and a long-lived fallback would preserve two playback protocols.

**Alternatives considered**:

- Permanent legacy reader: rejected because it retains the duplicate security/decoder path.
- Pretend converted packets are original: rejected because names and signatures would be fabricated after capture.

## Decision 10: Preserve original-signature trust evidence

**Decision**: Recording metadata binds and retains the Provider certificate name/digest and chain, trust-policy version, and authenticated session/capture interval used to validate the original packet after restart or certificate rotation.

**Rationale**: Re-signing historical packets would violate byte identity. Validating only against the current certificate can make unchanged recordings unverifiable after rotation or can accept the wrong signer. Historical validation must use the original chain and time binding under the configured trust anchor.

**Alternatives considered**:

- Re-sign packets during archival or replay: rejected because it changes canonical wire identity.
- Ignore certificate validity/identity for old packets: rejected as a trust bypass.
- Require the current Provider certificate forever: rejected because routine rotation would break durable playback.

## Decision 11: Optimize from truthful stage attribution, not one aggregate

**Decision**: Replace the misleading current `capture-to-decode` label with
`encoded-output-to-decoder-output`, add cursor-correlated monotonic events at
each owning stage, and require bounded-overhead matched evidence before an
algorithm or implementation optimization is accepted.

**Rationale**: Current code timestamps a chunk after FFmpeg has already encoded
it, then stops the timer only after the ground-station decoder emits JPEG. The
aggregate includes grouping, protection, signing, scheduling, transport,
validation, decryption, reorder and decoding, but excludes camera acquisition
and encoding. Without stage facts, a 180 ms residual cannot honestly be
assigned to cryptography, Mapping, networking or decoding. Local monotonic
spans remain valid without synchronized hosts; cross-host one-way subtraction
does not.

**Alternatives considered**:

- Keep only capture-to-decode: rejected because its name and attribution are
  false for the current timestamp origin.
- Add wall-clock timestamps and subtract across hosts: rejected without a
  measured offset/uncertainty bound.
- Trace every packet unconditionally: rejected because tracing can change the
  scheduler and become the performance regression being measured.
- Optimize the largest-looking component from one run: rejected because stage
  attribution, matched repetitions, correctness gates and negative-result
  retention are required for a defensible conclusion.
