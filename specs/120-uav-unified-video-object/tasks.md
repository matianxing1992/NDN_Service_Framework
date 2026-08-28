---

description: "Cohesive implementation tasks for one canonical UAV video object shared by live viewing and recording"

---

# Tasks: Unified Named UAV Video

**Input**: Design documents from `specs/120-uav-unified-video-object/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/unified-video-retention.md`, `quickstart.md`

**Tests**: Every behavior-changing task is test-first and closes through its focused gate. Test, implementation, and evidence remain in the same task unless security or dependency boundaries justify separation.

## Phase 1: Contract Foundation

**Purpose**: Freeze the packet-identity and timing evidence that every migration step must preserve.

- [X] T001 Freeze byte-exact canonical packet, Mapping-only, manifest/checkpoint, key-grant, and truthful performance-timeline fixtures before changing behavior: capture deterministic current LiveStream signed Data/Mapping wires and event order; prove the existing post-FFmpeg `captureMs` origin is `encoded-output-ready`; freeze stable cursor correlation, local monotonic/clock-uncertainty semantics and current p50/p95/p99 baseline; add failing vectors for same-name/different-wire, nested-packet mismatch, partial durability, wrong grant/session/signer, insufficient Mapping lead, reversed/duplicate/mixed-session/missing timing stages and unsafe cross-clock subtraction; document the baseline in `tests/fixtures/stream/`, `tests/unit-tests/stream.t.cpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, `tests/python/test_ndnsf_uav_unified_video.py`, and `specs/120-uav-unified-video-object/quickstart.md` (FR-001 through FR-004, FR-009 through FR-013, FR-021, FR-023, FR-024, FR-027 through FR-030; SI-001 through SI-005; SC-001, SC-003, SC-009, SC-010, SC-013).

- [X] T002 Implement the app-neutral immutable bounded published-packet feed and measurable Mapping/network timeline authority test-first in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, `tests/unit-tests/stream.t.cpp`, and `tests/python/test_ndnsf_core_streaming.py`: queue exact signed Mapping/source/optional-repair records after materialization without invoking APP code or waiting on the Face/I/O path, atomically give a late feed its retained snapshot/checkpoint plus future records without a race, report byte/packet/trace overflow gaps, reject mutation/session ambiguity, bound feed and trace state, use the shared stable cursor sampler and a dedicated `NDN_LOG` TRACE/DEBUG category with no direct console or per-packet INFO output, expose Mapping-available/payload-Interest-arrived/signed-and-materialized/Data-put ordering with local monotonic clock-domain metadata, and size the capped names-only Mapping horizon from measured RTT, production period, and jitter without embedding media (FR-003 through FR-006, FR-012, FR-014, FR-016, FR-017, FR-023, FR-025, FR-027 through FR-030; SC-001, SC-004, SC-009, SC-011, SC-013 through SC-015).

**Checkpoint**: Core can expose the one authoritative packet identity and prove whether an exact Interest arrived before production; it still knows nothing about UAV, H.264, keys, or Repo.

---

## Phase 2: User Story 1 - Publish One Video Data Product (Priority: P1) 🎯 MVP

**Goal**: Live delivery and recording share one canonical semantic name and signed encrypted packet.

**Independent Test**: A deterministic H.264 session with live and retention enabled yields byte-identical live/stored packet wires and exactly one encode, encryption, and signature per source.

- [X] T003 [US1] Replace the UAV raw recording writer with an APP-owned canonical retention worker and truthful Provider media-stage timeline test-first in `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, `NDNSF-UAV-APP/configs/uav_runtime.conf`, `tests/unit-tests/uav-protocol-state.t.cpp`, and `tests/python/test_ndnsf_uav_unified_video.py`: protect each H.264 source once, publish once, rename the existing post-FFmpeg timestamp to encoded-output readiness, record group-ready/protection-complete and owned queue spans using the stable cursor key without inventing acquisition time, drain the Core feed from a worker into existing RepoCore opaque `put/get` without adding media/storage protocol, make late retention begin from the next Mapping-covered SPS/PPS/IDR safe join, keep optional repairs transport-only, and prove live-only, recording-only, combined, late-start, multi-consumer, feed-bound, one-encode/one-encryption, wire/digest identity and bounded trace outcomes (FR-001 through FR-006, FR-012, FR-014, FR-020, FR-021, FR-025, FR-027 through FR-030; SI-001, SI-002; SC-001, SC-004, SC-008, SC-011, SC-013 through SC-015).

- [X] T004 [US1] Deliver the security-sensitive durable manifest/checkpoint, epoch-key authorization, and archived trust-evidence contract test-first in `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/configs/uav-stream-trust-schema.conf`, `tests/unit-tests/uav-protocol-state.t.cpp`, `tests/run_uav_stream_security_contract.py`, and `tests/python/test_ndnsf_uav_unified_video.py`: commit visibility only after every referenced Mapping/source wire is durable, sign and version manifests with gaps/completion, packet digests, original Provider certificate chain/digest, trust-policy version and authenticated capture interval, issue recipient/permission/Provider/session/key-epoch-bound live/history grants for the same per-epoch key, rotate or withhold future epochs on revocation without claiming retroactive erasure, and reject plaintext secret persistence, rollback, partial commit, certificate mismatch/rotation abuse, wrong authority, stale grant, and mixed-session references (FR-007, FR-009 through FR-013, FR-015 through FR-017, FR-021, FR-026; SI-003, SI-005; SC-002, SC-003, SC-012).

**Checkpoint**: New recordings contain no recording-only media chunks; their manifest discovers and authorizes the same packets used live.

---

## Phase 3: User Story 2 - Replay the Same Trusted Objects (Priority: P2)

**Goal**: Historical playback serves retained packet wires unchanged through the normal LiveStream/UAV admission and decoder path.

**Independent Test**: After process restart, beginning-to-end replay matches every committed live name/wire digest and decoded H.264 digest, while all malformed/tampered cases stop before decoder admission.

- [X] T005 [US2] Replace recording-only retrieval/decryption/reassembly with canonical replay and one live/replay Consumer timeline test-first in `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, `pythonWrapper/ndnsf/service.py`, `tests/unit-tests/uav-protocol-state.t.cpp`, and `tests/python/test_ndnsf_uav_unified_video.py`: validate manifest/grant and archived signer/chain/time evidence, reuse the existing stored-signed-packet producer to serve Mapping/source wires without re-signing, open the existing LiveStream consumer from the archived descriptor's beginning checkpoint, reuse the live name/signer/mapping/AES-GCM/replay/VideoPacket/decoder gate, record Data-received/signature-validated/decrypted/reorder-ready/decoder-input/decoder-output events under one stable cursor and local monotonic domain, report missing stages rather than zero, prove restart, certificate rotation, cache reuse and bounded trace overhead, reject gaps and every wrong name/digest/signer/session/key/checkpoint/trust/timeline case, then remove the raw recording fetch window, temporary-H264 reconstruction, duplicate decoder path and misleading capture-to-decode label without adding a second replay API (FR-006 through FR-013, FR-018 through FR-021, FR-026 through FR-030; SI-001, SI-003, SI-004; SC-002, SC-003, SC-006, SC-012 through SC-015).

**Checkpoint**: Live and recorded viewing differ only in source availability and starting range, not packet format or security/decoder logic.

---

## Phase 4: User Story 3 - Control Retention Independently (Priority: P3)

**Goal**: Viewing and retention keep independent lifecycle/status while sharing one authoritative publisher and packet set.

**Independent Test**: Viewer attach/detach, late recorder start, storage delay/failure, recorder restart, and shutdown preserve live behavior, bounded memory, monotonic checkpoints, and explicit gaps.

- [X] T006 [US3] Complete retention lifecycle, failure, diagnostics, and migration removal as one operational behavior in `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.*`, `NDNSF-UAV-APP/tools/uav_deployment_check.py`, `NDNSF-UAV-APP/configs/uav_runtime.conf`, `tests/unit-tests/uav-protocol-state.t.cpp`, and `tests/python/test_ndnsf_uav_unified_video.py`: make start/stop/restart and late safe-join attachment idempotent, bound drain/retry/queue state, expose separate publication/consumption/retention status and gaps, preserve live completion under storage failure, reject legacy raw-chunk DBs with export/cleanup guidance, and delete `recordRawChunk`, recording-only envelope/key-file/config/log fields and all remaining callers (FR-005, FR-013 through FR-021, FR-025; SI-005; SC-004 through SC-006, SC-011).

**Checkpoint**: One stream session supports zero or many viewers and optional retention without a second data representation or hidden failure.

---

## Phase 5: Integrated Acceptance And Documentation

**Purpose**: Prove the complete behavior on MiniNDN, decide whether Data-in-Data is necessary, and remove stale architecture claims.

- [X] T007 Run the complete correctness, performance-attribution, and adoption gate without rerunning failed candidate identities in `Experiments/NDNSF_UAV_Unified_Video_Minindn.py`, `tests/python/test_ndnsf_uav_unified_video.py`, `specs/120-uav-unified-video-object/quickstart.md`, `specs/120-uav-unified-video-object/completion-summary.md`, `README.md`, `README_ch.md`, `NDNSF-UAV-APP/README.md`, and `docs/NDNSF-UAV/slides/main.tex`: execute fresh 60-second live-only, recording-only, combined 0%, and combined 5%+storage-failure cells including late-start and certificate-rotation replay plus five tracing-off/on matched 0% pairs, then run one real visible GTK smoke that requires a nonblank image, GUI-owned decoded count, render-gate log, and screenshot; record completion, safe-join/feed continuity, exact live/stored wire identity, original-signer trust, encode/encryption counts, truthful event origins, clock uncertainty, per-stage sample/missing counts and p50/p95/p99, Mapping lead/future-hit, timeout/Nack, retention/trace lag and queue/checkpoint/gaps, CPU/memory/PIT, tracing overhead and security negatives; rank dominant measured stages and require any optimization claim to pass SC-016 rather than assigning the residual aggregate to crypto, network, or decoder by assumption; require at least 99% eligible Interest-before-production at 0% loss; if it passes, record inline Mapping as unnecessary and disabled, but if it fails after bounded tuning, stop with a Spec 119/120 amendment gate rather than silently enabling Data-in-Data, requiring exact canonical inner wires, independent inner/outer validation, wire-cap/recursion/downgrade/cache/amplification tests and SC-010 matched evidence before any implementation; synchronize English/Chinese docs and rebuild/visually inspect the UAV PDF (FR-021 through FR-031; SC-001 through SC-017).

---

## Dependencies & Execution Order

```text
T001 packet/security/timing fixtures
  -> T002 Core bounded packet feed + Mapping lead
      -> T003 canonical UAV retention
          -> T004 durable manifest + epoch-key grants
              -> T005 canonical replay and duplicate-path removal
                  -> T006 lifecycle/failure/migration closure
                      -> T007 MiniNDN adoption gate and docs
```

T003 and T004 share security-sensitive source files and execute sequentially. T005 depends on the durable format. T006 removes legacy behavior only after T005 proves canonical replay. T007 is the only campaign/docs closure and must not be fragmented into one task per cell, metric, or document.

## Parallel Opportunities

- Within T001, C++ packet fixtures and Python manifest/security fixtures can be prepared concurrently, but the task closes through one frozen-contract review.
- Within T007, completed result parsing and English/Chinese/slide updates may proceed concurrently only after all required cells have terminal artifacts.
- No source task is marked `[P]` because the current ownership surfaces overlap and premature parallel edits would create false packet authorities.

## Implementation Strategy

### MVP

Complete T001–T004. This yields one canonical live/retained packet and a trustworthy manifest, even before the Ground Station replay migration is exposed.

### Full delivery

Complete T005–T007, delete the duplicate recording path, run the frozen MiniNDN matrix, and make the Data-in-Data decision from evidence. A failed security, wire-identity, bounded-state, live-completion, or restart-replay gate blocks completion.

## Task Cohesion Review

- **Total tasks**: 7
- **Mechanical fragments coalesced**: tests, implementation, focused validation, evidence, and directly corresponding docs remain inside each behavioral task.
- **Security boundary retained**: manifest/key grants are separate from media retention because they carry materially different authority and review risk.
- **Migration boundary retained**: legacy deletion follows successful canonical replay and is not mixed into the initial writer change.
