# Contract: Unified Live And Recorded UAV Video

## 1. Non-negotiable wire identity

For every canonical source name `N` in session `S`:

```text
SHA256(live Data wire for N) == SHA256(retained Data wire for N)
```

The retained packet includes the original name, MetaInfo, encrypted Content, signature information, and signature value. Playback serves that wire unchanged. Re-encoding, re-encrypting, re-signing, or copying plaintext into another recording object violates the contract.

Mapping Data remains a signed names/checkpoint object in the default mode. A negotiated future version may carry the exact canonical signed source Data wire only under Section 7; it may not carry plaintext or invent a second payload representation.

## 2. App-neutral Core feed boundary

Proposed C++ shape:

```cpp
enum class LiveStreamPacketKind { Mapping, Source, Repair };

struct PublishedLiveStreamPacket {
  LiveStreamPacketKind kind;
  std::string streamId;
  uint64_t sessionEpoch;
  uint64_t mappingVersion;
  std::optional<StreamCursor> cursor;
  ndn::Name dataName;
  ndn::Name provider;
  ndn::Buffer signedDataWire;
  StreamContentDigest wireDigest;
  uint64_t materializedMonotonicMs;
};

struct PublishedPacketFeedOptions {
  StreamCursor fromCursor;
  size_t maxQueuedPackets;
  size_t maxQueuedBytes;
};

auto feed = publisher->openPublishedPacketFeed(options);
auto batch = feed->takeAvailable(maxItems); // called by an APP worker
```

Contract:

- Core queues a record only after immutable packet signing/materialization succeeds.
- Publication performs bounded internal enqueue only; no APP callback, Repo write, or blocking wait runs on the Face/I/O path.
- Feed overflow is explicit with the first/last omitted cursor and never changes the live packet.
- Feed creation, retained snapshot/checkpoint capture, and future-record handoff are atomic: no packet can fall between snapshot and feed.
- Core starts a late feed at the APP-supplied cursor when it is still retained, returns every required Mapping packet, and never claims unavailable history. Core does not interpret H.264; UAV APP supplies the next decoder-safe cursor.
- Mapping and source records preserve publication order within one session; durable checkpoint ordering remains APP responsibility.
- Feed consumers cannot modify the packet, Mapping, publisher state, or network response.
- Feed close/drop is idempotent and releases bounded retained references.
- Python parity exposes immutable records and feed polling without UAV/Repo/crypto types.

## 3. UAV retention adapter

The UAV APP attaches one recorder to a LiveStream publisher when recording is enabled:

```text
Core published-packet feed
  -> APP worker drains immutable wire records
  -> storage worker validates session/name/digest/order
  -> Repo put(exact signed wire)
  -> durable packet set
  -> atomic manifest/checkpoint update
```

The recorder:

- retains Mapping and Source packets; Repair retention is optional and marked transport-only;
- never calls H.264 encoder, payload encryptor, KeyChain sign, or alternate chunker;
- never blocks the Face/network event loop on storage;
- records queue overflow, storage error, missing Mapping, and shutdown timeout as explicit gaps;
- exposes queue depth/high-water, staged/durable frontiers, gap count, write latency, stored bytes, and digest mismatch count.

## 4. Naming and Repo layout

Canonical Mapping and source Data retain their original NDN names. Repo keys are derived from the full canonical Data name and store the full wire as opaque bytes.

Recording metadata uses a separate manifest namespace:

```text
/<provider>/UAV/Camera/Recording/<recording-id>/MANIFEST/v=<version>
```

The manifest may use a segmented catalog when the name/digest list exceeds one Data packet, but catalog segments contain metadata only. They never duplicate media content.

The manifest also binds the Provider signing-certificate name/digest, retained certificate chain, trust-schema version, and authenticated session/capture interval required to validate the unchanged historical packet. Replay validates the original signature against this archived evidence and the configured trust anchor; it never re-signs expired or rotated packets.

## 5. Key authorization

One key epoch protects each canonical media packet before LiveStream publication.

- Live camera-start/open response returns a recipient-authorized grant through the existing protected NDNSF response.
- Recording-manifest service returns history-permission grants for the permitted prior epoch set after normal permission checks.
- Each grant binds recipient, Provider, service/permission, stream/session, key epoch, and algorithm.
- Repo, manifest, catalog, logs, status, and evidence contain no plaintext key/salt.
- Revocation rotates or withholds future epochs. It cannot revoke an epoch key already legitimately learned for prior packets.

## 6. Replay

An authorized replay flow:

1. Fetch and validate manifest and key grant.
2. Register/route stored signed Mapping and source packet wires through the existing stored-packet producer boundary.
3. Open the existing LiveStream consumer in beginning/range mode with the archived descriptor/checkpoint.
4. Validate Mapping name/signature/continuity and exact payload name/Provider signature.
5. Apply the same UAV AAD, AES-GCM, session/key epoch, nonce/replay, VideoPacket, reorder, and decoder-safe admission used live.

No recording-only fetch/decrypt/reassembly/temporary-H264 decoder path remains.

## 7. Prefetch timing

Events are correlated by session/cursor; cross-host wall-clock subtraction is not required.

```text
insufficient lead:
  Mapping available -> 1/2 RTT -> exact Interest -> 1/2 RTT
  -> payload return -> 1/2 RTT
  ~= 3/2 RTT from simultaneous Mapping/payload availability

adequate lead:
  Mapping available >= RTT+jitter before payload
  -> exact Interest pending at Provider
  -> payload production -> 1/2 RTT return
```

The Mapping horizon must use measured RTT, sample period, and bounded jitter, remain capped, and report mapping-published, Interest-arrived, payload-produced ordering. An eligible 0% loss run passes only when at least 99% of source Interests arrive before production.

Only if that gate remains unsatisfied after bounded tuning may a versioned inline candidate be evaluated. Its outer Mapping entry binds cursor, inner semantic name, inner wire digest, Provider and session; both outer Mapping and inner Data are validated independently; nested Mapping, recursion, ambiguity, oversize, mixed-version downgrade, and inner/outer signer mismatch fail closed. The canonical inner packet must remain separately retrievable and cacheable under its semantic name. Adoption additionally requires the matched latency improvement and bandwidth/cache accounting in SC-010.

## 8. Failure and shutdown

- Live publication proceeds when retention rejects an enqueue; status records the gap/failure.
- Manifest visibility never advances past missing Mapping/source storage.
- Recorder restart resumes from the last durable checkpoint and rejects conflicting name/digest bindings.
- Shutdown stops new recording admission, drains within a bound, commits a final checkpoint or gap, wipes in-memory key copies, then releases Repo resources.
- Legacy raw-chunk DB detection returns an actionable unsupported-format error; no fallback parser enters canonical replay.

## 9. Performance attribution

The current timestamp recorded after `fread()` receives FFmpeg H.264 output is
named `encoded-output-ready`. It is not `capture` and excludes acquisition and
encoding. A true acquisition event is optional and must carry its provenance
and uncertainty through the pipeline.

Every selected item uses one stable correlation key:

```text
(streamId, sessionEpoch, cursor, frameId, segmentIndex)
```

Required event vocabulary, when applicable:

```text
source-acquired
encoded-output-ready
group-ready
protection-complete
signed-and-materialized
mapping-available
payload-interest-arrived
data-put
data-received
signature-validated
decrypted
reorder-ready
decoder-input
decoder-output
```

Events contain monotonic timestamp, process/clock-domain ID, sampler metadata,
and local queue state. A missing stage carries a reason. The aggregator may
subtract only events from one monotonic domain unless it also records an
acceptable clock-offset uncertainty bound. Otherwise it reports causal order
and RTT, not measured one-way latency.

The same `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` stable cursor selection applies at
Core and APP stages. Sampled events are emitted only through a dedicated
`NDN_LOG` TRACE/DEBUG category. Direct console I/O (`std::cout`, `std::cerr`,
`printf`, `fprintf`) and per-packet INFO logs are outside the contract. Trace
queues are bounded; drops are counted. Reports give
sample/missing counts, p50/p95/p99 local spans, truthful end-to-end spans,
queue high-water, CPU/memory, network/control counters, and tracing overhead.
Optimization evidence freezes the baseline, target stage, candidate identity
and matched input. Correctness and security gates remain blocking even when a
latency metric improves.

## 10. Removal inventory

After canonical retention passes migration gates, remove or replace:

- `VideoPublisher::recordRawChunk` and `recordSingleRawChunk`;
- recording-only `HybridMessageEnvelope` generation/decryption and `.content-key` file;
- `/recording/.../chunk/<index>` as a media payload format;
- raw chunk counters/manifest fields that imply a second video object;
- Ground Station recording-only fetch window, chunk map, temporary `.h264` assembly, and separate decoder path;
- related configuration, tests, logs, and slide text.

Keep the recording manifest/catalog service, but redefine it as discovery and authorization over canonical packet names.
