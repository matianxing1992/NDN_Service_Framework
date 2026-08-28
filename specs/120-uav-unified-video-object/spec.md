# Feature Specification: Unified Named UAV Video

**Feature Branch**: `Experimental`

**Created**: 2026-07-18

**Status**: Draft

**Input**: Replace the UAV application's separate live-video and recording object formats with one canonical, semantically named H.264 data product that supports live viewing, durable retention, and later playback without duplicating the media payload.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Publish One Video Data Product (Priority: P1)

As a UAV application developer, I want each captured H.264 media unit to become one immutable named data object so that live viewing and recording do not create different chunks, names, encryption envelopes, or signatures for the same media.

**Why this priority**: The duplicate live/recording representations are the root architecture defect. Removing them establishes one source of truth for every later use.

**Independent Test**: Capture a deterministic H.264 fixture with live viewing and recording enabled, then prove that the object delivered live and the object retained for the same semantic name are byte-identical signed packets and decode to the same media bytes.

**Acceptance Scenarios**:

1. **Given** recording and live viewing are enabled for one camera session, **When** a media unit is published, **Then** the live path and retention path reference the same semantic data name and byte-identical signed packet.
2. **Given** no live consumer is attached, **When** recording is enabled, **Then** the same canonical named objects continue to be produced and retained without creating a recording-only media format.
3. **Given** recording is disabled, **When** live consumers are attached, **Then** canonical publication continues without storage-specific fields or behavior changing the media object.
4. **Given** optional recovery data is enabled, **When** a source object is retained, **Then** the source remains canonical and recovery objects are classified separately as transport aids rather than alternate recordings.

---

### User Story 2 - Replay the Same Trusted Objects (Priority: P2)

As a ground-station operator, I want recorded playback to retrieve the same named and signed video objects that were available live so that replay uses the same authenticity, confidentiality, ordering, and decoding checks.

**Why this priority**: Unified production has limited value if playback still reconstructs a second raw-chunk protocol or bypasses the live validation path.

**Independent Test**: Retain a finite stream interval, stop the live session, restart the playback components, discover the recording, and play it from retained packets while proving name, packet digest, signer, mapping, and decoded H.264 output match the original live publication.

**Acceptance Scenarios**:

1. **Given** a completed recording, **When** an authorized operator opens it, **Then** playback discovers its time/cursor range and consumes the retained canonical packets through the same validation and media-admission rules used for live delivery.
2. **Given** a retained packet, mapping object, manifest, or key authorization has been altered, **When** playback reaches it, **Then** the object is rejected without feeding unverified bytes to the decoder.
3. **Given** live and historical access have different permissions, **When** an authorized historical user opens the recording, **Then** access is granted without changing or re-encrypting the stored media packets.
4. **Given** only an old recording-only chunk database exists, **When** it is opened after this feature, **Then** the system reports the unsupported legacy format clearly and does not silently mix it with canonical recordings.

---

### User Story 3 - Control Retention Independently (Priority: P3)

As an operator, I want live consumption and durable retention to have independent lifecycles and visible health while sharing one data product, so that stopping a viewer does not stop recording and a storage failure does not masquerade as a camera or network failure.

**Why this priority**: Lifecycle independence remains useful; only the duplicated data representation must disappear.

**Independent Test**: Start one publication session, attach and detach multiple live consumers, start and stop retention, inject storage delay/failure, and verify one publisher remains authoritative while status distinguishes publication, consumption, and retention outcomes.

**Acceptance Scenarios**:

1. **Given** recording is active, **When** the last live viewer disconnects, **Then** canonical publication and retention continue until the application stops them.
2. **Given** live viewing is active, **When** retention is stopped or its storage becomes unavailable, **Then** live delivery continues and the recording reports a bounded, explicit gap or failure.
3. **Given** a restarted recorder or playback producer, **When** it resumes from a committed checkpoint, **Then** it never assigns a second packet to an existing semantic name or claims uncommitted objects.
4. **Given** several consumers, **When** they request the same semantic name, **Then** they observe one immutable packet identity rather than per-consumer variants.

### Edge Cases

- The recorder starts after publication has already begun or stops before the session ends.
- Publication succeeds but durable storage fails before the recording checkpoint commits.
- Storage commits a packet but crashes before the manifest checkpoint advances.
- A manifest references missing, duplicate, out-of-range, wrong-session, or wrong-signer objects.
- Mapping and media packets become visible in different orders.
- Mapping arrives less than one network round trip before its media packet, so the exact payload Interest cannot reach the publisher before production.
- A consumer joins at the latest decoder-safe point while a recorder retains the full session.
- Optional recovery objects exist although every canonical source object was retained.
- Retention queue pressure exceeds its configured bound while the live path remains healthy.
- A content key authorization is missing, stale, revoked, or bound to another session.
- A user previously received a valid key epoch and is later revoked; only future epochs can be withheld.
- Existing recording-only databases contain raw encrypted chunks from the retired format.
- A metric is labelled capture-to-decode even though its origin is the first
  encoded H.264 byte read from FFmpeg rather than camera acquisition.
- Producer and consumer clocks have unknown offset or uncertainty, making a
  cross-host one-way subtraction invalid even though local stage durations are valid.
- One frame spans several chunks, queue waits, or decoder writes, so attributing
  the whole delay to encryption or networking would double-count work.
- Timeline sampling drops or selects different cursor IDs at different stages,
  producing a biased performance profile.
- Detailed tracing itself changes scheduling, CPU load, or latency enough to
  invalidate the optimization comparison.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Each captured H.264 media unit MUST have one canonical semantic data name and one immutable signed packet representation per publication session.
- **FR-002**: Live delivery and durable retention MUST reference the same canonical source packet; neither path may independently re-chunk, re-encrypt, rename, or re-sign the media payload.
- **FR-003**: The canonical packet MUST bind its semantic name, publisher identity, stream/session identity, ordering cursor, media metadata, and protected payload so that substitutions fail validation.
- **FR-004**: Sequential cursors MUST remain internal ordering and prefetch aids; retained and live source packets MUST preserve their meaningful application names.
- **FR-005**: The system MUST support publication with zero, one, or multiple live consumers and with retention independently enabled or disabled.
- **FR-006**: Retention MUST store the byte-exact canonical signed source packets plus the signed mapping/checkpoint material required to resolve and replay them.
- **FR-007**: A recording manifest MUST identify the publisher, session, media format, committed cursor/time ranges, mapping checkpoint, canonical packet identities/digests, authorization material reference, gaps, and completion state without embedding a second copy of media bytes.
- **FR-008**: Historical playback MUST fetch retained canonical packets by their original names and apply the same signer, name, mapping, session, replay, authenticated-decryption, and decoder-admission checks as live consumption.
- **FR-009**: The system MUST use one protected media payload for all consumers. For each packet key epoch, different live and historical permissions MUST be expressed by recipient-specific authorization of the same epoch key, not by re-encrypting the media payload; historical access may grant a permitted set of prior epochs.
- **FR-010**: Plaintext media keys MUST NOT be stored in recording manifests, repository metadata, logs, status objects, or catalogs.
- **FR-011**: Every key authorization MUST bind the intended service/permission, publisher, stream/session, key epoch, and recipient authority and MUST fail closed when any binding is missing or mismatched.
- **FR-012**: Optional recovery packets MUST remain separately named, signed, bounded transport objects; they MUST NOT replace canonical source packets in the recording manifest or become a second recording format.
- **FR-013**: Retention visibility MUST advance only through an explicit committed checkpoint after all referenced source and mapping packets are durable; partial work MUST be reported as incomplete or gapped.
- **FR-014**: Publication MUST NOT block the network I/O loop on repository work; retention buffering, concurrency, retries, and memory use MUST be bounded and observable.
- **FR-015**: A retention failure MUST NOT silently stop a healthy live publication. The application MUST expose whether policy chose to continue with an explicit recording gap or stop the recording.
- **FR-016**: Start, stop, retry, and restart operations MUST be idempotent and MUST preserve immutable name-to-packet bindings across concurrent consumers and recorder recovery.
- **FR-017**: The application MUST expose separate publication, live-consumption, and retention status while identifying their shared stream/session and checkpoint.
- **FR-018**: New recordings MUST use only the canonical format. The retired recording-only writer and duplicate playback/decryption path MUST be removed after fixture-backed migration validation.
- **FR-019**: Existing recording-only databases are not automatically migrated. They MUST be detected and rejected with an actionable export/cleanup message rather than interpreted as canonical recordings.
- **FR-020**: The unified path MUST preserve latest-safe live join, beginning/range replay, optional recovery, exact-name retrieval, cache reuse, and multi-consumer behavior already provided by the live-stream mechanism.
- **FR-021**: Tests MUST cover byte identity, tampering, wrong signer/session/key authorization, partial commit, restart, bounded storage failure, multiple consumers, live-only, recording-only, and combined operation.
- **FR-022**: Network acceptance MUST use fresh MiniNDN runs with a 60-second measured window per required cell and MUST retain negative outcomes rather than tuning or rerunning them away.
- **FR-023**: Names-only Mapping MUST remain the default and its lead MUST adapt to measured round-trip time, production period, and bounded jitter so exact payload Interests can reach the publisher before production. A versioned Data-in-Data mode MAY be considered only when matched evidence proves adequate Mapping lead cannot meet the latency objective and the mode passes the additional security, wire-size, cache, and amplification gates in FR-024.
- **FR-024**: Any enabled Data-in-Data mode MUST embed the exact complete canonical signed source Data wire, never a second payload format. Consumers MUST independently validate outer Mapping and inner Data signatures plus cursor/name/digest/Provider/session binding; reject ambiguity, substitution, recursion, oversize, or mixed-mode downgrade; and preserve ordinary retrieval under the inner semantic name.
- **FR-025**: A retention session attached after publication begins MUST atomically establish its starting checkpoint and future packet observation at the next Mapping-covered decoder-safe H.264 join, or fail clearly. It MUST NOT omit required Mapping, claim pre-join packets, or begin from an undecodable delta-frame boundary.
- **FR-026**: A recording MUST retain and bind the Provider certificate name/digest, certificate chain, trust-policy version, and authenticated session/capture interval needed to validate the original unchanged packet signatures after process restart or certificate rotation. Playback MUST fail clearly rather than re-sign or weaken validation when this evidence is unavailable or inconsistent.
- **FR-027**: The implementation MUST expose a correlated per-session/cursor performance timeline covering, when applicable, source acquisition, encoded-output readiness, group/segment readiness, protection completion, signing/publication, Mapping availability, payload-Interest arrival, Data transmission/reception, validation, decryption, reorder/reassembly, decoder input, and decoder output. Missing or inapplicable stages MUST be explicit rather than estimated.
- **FR-028**: Every latency metric MUST name its actual start and end events. The existing timestamp taken after reading encoded H.264 output MUST be reported as encoded-output readiness and MUST NOT be described as camera capture time. A true capture-to-decode metric MAY be reported only when an authenticated acquisition timestamp survives the pipeline.
- **FR-029**: Performance attribution MUST use monotonic same-process durations for local stages and MUST record clock-offset uncertainty before subtracting timestamps across nodes. When the uncertainty bound is unavailable or too large, the system MUST report RTT and causal ordering instead of claiming measured one-way delay.
- **FR-030**: Performance traces MUST use the shared stable request/cursor sampler, bounded buffers, and a documented sampling rate. Runtime event emission MUST use a dedicated `NDN_LOG` category and sampled TRACE/DEBUG records; `std::cout`, `std::cerr`, `printf`, `fprintf`, per-packet INFO logging, or other direct synchronous console output MUST NOT be used for performance timelines. Reports MUST include per-stage counts, missing-stage counts, p50/p95/p99, queue high-water marks, CPU/resource cost, log-drop counts, and tracing overhead. An algorithm or implementation optimization claim MUST identify the measured dominant stage, compare a frozen matched baseline and candidate, preserve correctness/security gates, and retain negative outcomes.
- **FR-031**: A live-view acceptance run MUST exercise the real GTK display path from canonical stream-ready status through the frame callback, Pixbuf submission, visible image widget, and GUI-owned decoded-frame counter. A ServiceContainer decoder count alone MUST NOT be reported as proof that the operator can see video.

### Security Invariants

- **SI-001 — One packet identity**: One semantic source name identifies exactly one signed packet wire within a session, whether served live, from cache, or from durable storage.
- **SI-002 — Encrypt once**: Media plaintext is protected once before publication; retention stores the resulting signed packet unchanged.
- **SI-003 — Authorize keys, not copies**: Permission differences change only recipient-specific key authorization, never the stored media packet.
- **SI-004 — Same admission gate**: No live or replay byte reaches the decoder without the same name, signer, session, mapping, authenticated-decryption, and replay checks.
- **SI-005 — Committed visibility**: A recording may advertise only packets and mappings proven durable at its committed checkpoint.

### Key Entities

- **Canonical Video Packet**: The immutable, semantically named, publisher-signed and protected representation of one H.264 media unit.
- **Stream Mapping**: Signed cursor-to-semantic-name bindings used for ordering and prefetch without replacing the canonical name.
- **Recording Manifest**: Signed durable description of one retained interval and its committed canonical packet/mapping set, gaps, authorization reference, and completion state.
- **Content-Key Authorization**: Recipient- and permission-bound grant that reveals the session content key without copying or changing media packets.
- **Retention Checkpoint**: Monotonic proof of the highest complete durable range; it never includes staged or partially stored work.
- **Retention Gap**: Explicit range that could not be stored and must not be represented as complete.
- **Performance Timeline**: A sampled, cursor-correlated sequence of truthful local and network-boundary events used to separate Mapping lead, media processing, security work, queueing, transport, and decoder delay without assuming synchronized clocks.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: For every combined live-and-recorded fixture packet, the live packet wire and retained packet wire are byte-identical, with zero duplicate media names or alternate recording payloads.
- **SC-002**: A retained session replays after component restart with 100% agreement in semantic name, packet digest, publisher identity, ordering, and decoded H.264 bytes for all committed source packets.
- **SC-003**: All tampered name, packet, mapping, manifest, signer, session, key-authorization, and checkpoint cases are rejected before decoder admission.
- **SC-004**: Live-only, recording-only, combined, late-recorder, viewer-detach, and storage-failure scenarios complete without creating a second media encoding or unbounded queue.
- **SC-005**: Under injected storage failure, live publication retains its matched no-recorder completion behavior while retention reports every affected range as failed or gapped.
- **SC-006**: After migration, source inspection and runtime diagnostics show zero calls to the retired raw recording chunk writer, recording-only encryption envelope, and duplicate recording decoder path.
- **SC-007**: Required fresh MiniNDN cells run for 60 measured seconds and report completion rate, delivery failures, p50/p95 live delay, retention lag, queue high-water mark, stored/live digest equality, and resource use.
- **SC-008**: With identical workload and settings, enabling retention performs one H.264 encode and one media-payload encryption per canonical source object; any additional cost is limited to bounded storage and metadata work.
- **SC-009**: In the required 0% loss MiniNDN cells, at least 99% of eligible canonical source packets have an exact-name Interest pending at the publisher before production, demonstrating that steady-state production-to-delivery adds no Mapping discovery round trip; insufficient-lead cells are reported separately rather than counted as successful prefetch.
- **SC-010**: Data-in-Data remains disabled when SC-009 passes. If SC-009 cannot pass after bounded lead tuning, an explicitly versioned candidate is admissible only if all nested-packet security negatives pass, every outer packet fits the signed wire cap, the inner wire is byte-identical to the canonical packet, p95 delivery delay improves by at least 10% in four of five matched pairs, and bandwidth/cache costs are reported without hiding regressions.
- **SC-011**: Every late-start retention fixture either begins at a verified decoder-safe join and replays successfully after restart with complete required Mapping, or fails before advertising a playable recording.
- **SC-012**: Certificate-rotation and historical-certificate fixtures validate every retained packet against its original signer and archived trust evidence with zero packet rewrites; missing, expired-at-capture, mismatched, or unanchored evidence is rejected before decoder admission.
- **SC-013**: Deterministic fixtures produce complete, monotonically ordered timelines for every required sampled cursor, identify the current post-encoder timestamp as encoded-output readiness, and reject fabricated, reversed, duplicate, mixed-session, or ambiguously named timing events.
- **SC-014**: Every accepted 60-second MiniNDN cell reports sample counts and p50/p95/p99 for available provider processing, network/causal, consumer security, reorder/queue, decoder, and encoded-output-to-decoder-output intervals, together with clock uncertainty, missing-stage counts, CPU/memory, and queue high-water marks; no unavailable stage is represented as zero time.
- **SC-015**: At the configured production sampling rate, tracing-on versus tracing-off changes p95 encoded-output-to-decoder-output latency and CPU consumption by no more than 5% in at least four of five matched 0% loss pairs, or the evidence is rejected and the sampling rate is reduced before optimization experiments.
- **SC-016**: Each accepted optimization claim names its target stage and changes either that stage's p95 or end-to-end p95 by at least 10% in at least four of five matched pairs without worsening completion, security, timeout/Nack, bounded-state, or packet-identity gates; otherwise it is preserved as a negative or inconclusive result.
- **SC-017**: One fresh automated MiniNDN GUI smoke displays a nonblank decoded video frame in the actual GTK image widget, records a GUI-owned decoded-frame count of at least three, emits `AUTO_VIDEO_GUI_RENDER_GATE status=PASS`, and retains a screenshot. The launcher fails when this GUI-specific gate is absent even if the ServiceContainer decoder reports frames.

## Assumptions

- Spec 119 LiveStream semantic names, signed Mapping, exact-name retrieval, latest/beginning start, and optional recovery remain the transport foundation.
- Spec 118 UAV H.264 boundary detection, authenticated encryption, descriptor authorization, and decoder admission remain authoritative and are consolidated rather than replaced.
- NDNSF-DistributedRepo stores opaque packet wires and metadata; it does not learn H.264, media keys, or UAV policy.
- The UAV application owns capture, H.264 boundaries, retention policy, content-key authorization, manifest semantics, and user-facing lifecycle status.
- NDNSF Core may expose an application-neutral committed-packet observation/retention boundary but MUST NOT depend on Repo or UAV types.
- Existing local recording-only databases are experimental artifacts and have no automatic compatibility guarantee; operators receive a clear export/cleanup instruction.
- A recipient that legitimately learns an epoch key can continue decrypting packets from that epoch; revocation rotates or withholds future epoch grants and is not represented as retroactive cryptographic erasure.
- Real-radio and camera-hardware validation remains outside Spec 120; MiniNDN plus deterministic H.264 fixtures is the acceptance environment.

## Out of Scope

- Changing the H.264 encoder, bitrate adaptation algorithm, camera-control service, or UAV flight-control behavior.
- Treating the repository as a live-stream protocol or adding Repo knowledge to NDNSF Core.
- Re-encrypting old recording-only databases into the canonical format automatically.
- Claiming that retention improves live latency, recovery, or video quality.
- Replacing Spec 119 Mapping/prefetch or Spec 118 encryption with a new streaming protocol without an explicit versioned amendment and its security/adoption gates.
