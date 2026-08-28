# Feature Specification: UAV Video True End-to-End Latency

**Feature Branch**: `Experimental`

**Created**: 2026-07-19

**Status**: Draft

**Input**: Measure the real time from a camera frame becoming available at the Provider to that same frame being presented by the Ground Station, then use per-stage evidence to remove avoidable decoder, buffering, conversion, and GUI delay without weakening NDNSF Streaming.

## User Scenarios & Testing

### User Story 1 - Trustworthy Capture-to-Presentation Measurement (Priority: P1)

As a UAV operator or researcher, I can see the true age of a displayed frame and the time spent at every available stage, rather than a sum of unrelated percentiles.

**Why this priority**: The current timestamp starts after encoded bytes appear, and one encoded group may yield several decoded frames. Optimization decisions are unsafe until the original frame and displayed frame can be proven identical.

**Independent Test**: Run a deterministic 30 FPS source through the complete MiniNDN video path and verify that sampled source-frame identities survive capture, encoding, NDNSF delivery, decoding, and GUI presentation without FIFO guessing or cross-clock claims.

**Acceptance Scenarios**:

1. **Given** a source frame becomes available, **When** it is encoded, transported, decoded, and presented, **Then** one immutable frame identity and its capture-origin timestamp remain attributable at every supported stage.
2. **Given** one encoded access unit yields zero, one, or several decoder outputs, **When** the report is generated, **Then** only codec-proven associations are reported and every ambiguous interval is unavailable rather than fabricated.
3. **Given** stages use clocks without bounded synchronization uncertainty, **When** capture-to-presentation is reported, **Then** the report states the clock limitation and does not claim an exact one-way duration.
4. **Given** a rendering backend cannot observe compositor scan-out, **When** GUI latency is reported, **Then** it is labelled as widget submission rather than physical presentation.

---

### User Story 2 - Smooth Low-Latency Live Display (Priority: P2)

As a UAV operator, I see current video quickly after starting a stream and do not experience periodic multi-frame freezes during steady playback.

**Why this priority**: The provisional current code path reduced decoder startup to 87 ms in one exploratory pair, but a 142 ms p99 decoder-output gap remains, Interest work rose about 8.3 times, and the current MJPEG/Pixbuf path performs avoidable conversion and GUI scheduling.

**Independent Test**: Under a frozen 60-second, zero-loss, 30 FPS MiniNDN workload, compare one-variable pipeline candidates and show that the retained path reduces true frame age and tail stalls while maintaining continuity and frame completion.

**Acceptance Scenarios**:

1. **Given** a new live session, **When** the first decodable key frame arrives, **Then** decoder initialization is bounded and reported separately from steady playback.
2. **Given** the decoder and GUI are slower than production, **When** new frames arrive, **Then** bounded queues prefer the newest decodable frame and stale work cannot accumulate without limit.
3. **Given** a candidate removes an intermediate conversion or copy, **When** it is evaluated, **Then** the same authenticated video content reaches the display and rollback remains available.
4. **Given** a reconnect or mid-stream latest join, **When** decoding resumes, **Then** codec configuration and a decodable key-frame boundary are available without weakening Stream validation.

---

### User Story 3 - Evidence-Driven Backend and Default Selection (Priority: P3)

As a maintainer, I can choose the simplest video pipeline that meets latency, continuity, security, Interest-efficiency, and resource gates on the supported platform.

**Why this priority**: A mature media backend may improve timestamps and rendering, but a new dependency or hardware path is justified only by matched measurements and a bounded fallback.

**Independent Test**: Run five matched baseline/candidate pairs plus trace-on/off controls using unique identities and frozen conditions; retain only candidates that pass all correctness and operational gates in at least four pairs.

**Acceptance Scenarios**:

1. **Given** several plausible bottlenecks, **When** experiments run, **Then** each cell changes exactly one declared variable and preserves failures as measured outcomes.
2. **Given** a candidate lowers latency but increases dropped frames, unnecessary Interests, CPU, memory, queue depth, or startup failures beyond its gate, **When** audited, **Then** it is rejected or remains explicitly experimental.
3. **Given** the retained backend is unavailable at startup, **When** fallback is allowed, **Then** the operator receives a visible reason and the established legacy path can be selected explicitly without silent behavior drift.

### Edge Cases

- The consumer joins between key frames or before codec configuration is available.
- A captured frame is dropped by the encoder, decoder, newest-frame queue, or GUI before presentation.
- One source frame produces multiple access units, or one access unit produces multiple displayable frames.
- Frame identity metadata is absent, duplicated, malformed, unauthenticated, or belongs to a retired session.
- Provider and consumer clocks have unknown offset or drift.
- The GUI event loop is blocked for several refresh periods.
- The stream is stopped or restarted while decoder or renderer callbacks remain queued.
- Hardware acceleration is unavailable, changes decoded output, or initializes more slowly than software decoding.
- FEC recovery, loss, retransmission, or late Mapping changes arrival order without changing source identity.

## Requirements

### Functional Requirements

- **FR-001**: Each source frame MUST receive a session-scoped immutable identity and capture-origin timestamp at the earliest observable acquisition boundary, before encoding or provider batching.
- **FR-002**: The source-frame identity MUST remain integrity-bound through codec metadata or an equivalently verifiable presentation mapping, NDNSF protected payload metadata, decoder output, and GUI submission; queue position or coincident sequence numbers MUST NOT establish identity.
- **FR-003**: The system MUST record capture/acquisition, encode admission/output, packetization, protection, Data publication, Data receipt, validation, decryption, reorder readiness, decoder input/output, GUI submission, and observable presentation boundaries with explicit clock authority.
- **FR-004**: Reports MUST provide counts and p50/p95/p99 for every valid stage and the total capture-to-widget or capture-to-presentation interval; missing, ambiguous, cross-clock-unsafe, or unsampled intervals MUST remain unavailable.
- **FR-005**: Decoder startup, reconnect/join, warmup, and steady-state samples MUST be separated; an output interval MUST NOT be presented as per-frame processing latency.
- **FR-006**: Trace identity and timestamps MUST be bounded, sampled with `NDNSF_TIMELINE_TRACE_SAMPLE_RATE`, emitted through `NDN_LOG`, and removable from hot paths when tracing is disabled; direct per-frame console output is prohibited.
- **FR-007**: The live pipeline MUST keep decoder and display queues bounded, expose queue depth and drop reason, and prefer current decodable video over stale queued frames.
- **FR-008**: The pipeline MUST provide codec configuration and authenticated key-frame readiness for startup, reconnect, and latest join without changing semantic Data names or trusting unsigned hints.
- **FR-009**: Pipeline candidates MAY include persistent/prewarmed decoding, low-delay encoder/decoder settings, codec-aware packet boundaries, hardware acceleration, removal of intermediate MJPEG/Pixbuf conversion, and a direct video rendering path; each MUST remain independently selectable for one-variable evaluation.
- **FR-010**: The currently functional FFmpeg pipe path MUST remain an explicit bounded rollback until a replacement passes the full acceptance matrix; fallback MUST be visible and MUST NOT occur silently during a measured cell.
- **FR-011**: NDNSF Streaming MUST retain semantic payload names, signed Mapping, exact-name future Interests, Provider validation, AES-GCM/replay checks, and optional ciphertext FEC; this feature MUST NOT introduce Data-in-Data or move codec/application policy into Stream Core.
- **FR-012**: Experiments MUST use the same source, resolution, frame rate, bitrate, GOP, FEC mode, topology, delay/loss, warmup, measured duration, logging level, and resource limits across each matched pair.
- **FR-013**: Every performance cell MUST report completion, decoded and displayed frames, frame drops by stage, startup, capture-to-display availability, stage percentiles, output-gap percentiles, future-Interest efficiency, CPU, memory, PIT, and queue high-water marks.
- **FR-014**: Retention decisions MUST use five matched 60-second baseline/candidate pairs and five matched trace-off/on pairs with a precommitted counterbalanced execution order, unique identities, no automatic reruns, explicit stop conditions, and preserved negative evidence. Exploratory candidate-selection cells and prior Spec 121 cells MUST NOT be reused as confirmatory acceptance cells.
- **FR-015**: Public NDNSF Stream and UAV camera-control contracts MUST remain compatible unless a separately audited contract change is necessary to carry authenticated frame identity; any such additive change MUST be versioned and reject malformed or stale identities.

### Key Entities

- **Source Frame Identity**: Session epoch, source-frame ID, capture-origin timestamp, clock authority, codec presentation timestamp, key-frame/configuration state, and integrity binding.
- **Video Stage Event**: Frame identity, stage, timestamp, clock authority, queue depth, outcome, and optional drop reason.
- **Presentation Observation**: Decoded-frame identity, GUI submission time, observable presentation time or limitation, refresh context, and stale-frame decision.
- **Pipeline Candidate**: Backend and one changed variable, capability result, rollback selection, and immutable configuration digest.
- **Matched Experiment Cell**: Frozen workload and topology, unique identity, stage distributions, correctness/resource counters, completion status, and admissibility decision.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic tests preserve the exact source-frame identity through every supported stage for 100% of sampled displayed frames and reject 100% of ambiguous, duplicate, stale-session, or unauthenticated bindings.
- **SC-002**: Every accepted report labels capture origin, clock authority, render endpoint, startup/steady classification, sample counts, and unavailable boundaries; no report obtains an end-to-end value by adding unrelated percentiles.
- **SC-003**: In the same-host MiniNDN acceptance environment, at least 95% of sampled displayed frames have a valid capture-to-widget-submission measurement; physical presentation is claimed only when directly observed.
- **SC-004**: The retained default reaches first displayable output within 150 ms of first decoder input in at least four of five matched runs.
- **SC-005**: During the steady 30 FPS window, the retained default keeps decoder-output-gap p99 below 67 ms and has no unexplained stall above 100 ms in at least four of five matched runs.
- **SC-006**: The retained default improves true steady capture-to-widget p95 by at least 25% in four of five matched pairs and reaches at most 150 ms p95 and 250 ms p99 in the same-host zero-loss environment.
- **SC-007**: At least 99% of produced displayable frames are either displayed or have one explicit bounded drop reason; unexplained loss is zero and displayed-frame completion decreases by no more than 1% versus baseline.
- **SC-008**: Future exact-Interest hit ratio remains at least 99%, and payload/Interest work per produced displayable frame is no more than 2 times the frozen `legacy-pipe + stdio-batched` rollback baseline measured with the same Spec 122 identity instrumentation. A candidate exceeding either bound MUST NOT become the runtime default; it MAY remain explicitly experimental with its measured tradeoff preserved.
- **SC-009**: Retained changes increase user CPU by no more than 15%, memory by no more than 10%, max PIT by no more than 10%, and keep all media/GUI queues within declared bounds in at least four of five pairs.
- **SC-010**: Trace-on versus trace-off changes steady capture-to-widget p95 and CPU by no more than 5% in four of five matched pairs.
- **SC-011**: Focused unit, codec-correlation, security/protocol, GUI, and MiniNDN regressions pass without weakening the Spec 118/119/121 invariants.

## Assumptions

- MiniNDN same-host runs provide the first exact cross-role clock domain; real multi-host one-way measurements remain unavailable until clock-offset uncertainty is bounded.
- A deterministic camera-like 30 FPS source is the acceptance source; V4L2 and hardware decoder probes are capability extensions, not prerequisites for correctness.
- The POSIX-read plus 20 ms partial-flush path is the provisional current code default and the starting baseline, not an accepted performance default; `NDNSF_UAV_ENCODER_PIPE_READ_MODE=stdio-batched` is the legacy rollback reference.
- The APP owns camera, codec, decoder, rendering, and queue policy. Stream Core owns Mapping, exact-name fetching, validation, bounded transport state, and optional FEC.
- Hardware-specific acceleration is retained only when software fallback and equivalent evidence are available.
- GPU, Docker, iTiger, and outdoor UAV trials are outside this feature.
