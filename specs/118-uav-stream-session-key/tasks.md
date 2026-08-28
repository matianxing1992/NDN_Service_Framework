---
description: "Cohesive UAV encryption/adaptation tasks over the Spec 119 LiveStream API"
---

# Tasks: UAV Stream Session-Key Delivery

## Architecture Boundary

Spec 118 owns only the UAV camera-start key contract, semantic video-name
planning, AES-GCM protection/admission, H264 safe join, sample/reorder/decoder,
and UAV lifecycle/UI integration. Spec 119 owns Mapping publication/retrieval,
Provider validation, Face/timer scheduling, future exact Interests, controller
policy, bounded pending state, default-off opaque-byte FEC, and the public
C++/Python LiveStream API.

## Phase 1: Frozen UAV Cryptographic Contract

- [X] T001 Freeze and implement deterministic UAV descriptor/versioned-name contracts, explicit-nonce AES-256-GCM, `salt || uint64_be(cursor)` nonce derivation, canonical TLV AAD, strict envelope guard, and C++ golden/malformed/remap/nonce-reuse vectors in `ndn-service-framework/HybridMessageCrypto.hpp`, `ndn-service-framework/HybridMessageCrypto.cpp`, `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, and `tests/unit-tests/uav-protocol-state.t.cpp`; prove five-frontier ordering, exact key/salt parsing, cursor/original-name/Provider/service/Stream/session/Mapping/key binding, complete VideoPacket round trip, wrong-valid-Provider rejection, malformed envelope rejection, and unchanged generic envelope/Response wire (FR-002, FR-003, FR-005 through FR-010, FR-015, FR-018, FR-020; SC-004, SC-007).

**Checkpoint**: Crypto and descriptor bytes are frozen. Runtime integration is
blocked until Spec 119 T003/T004 completes the public API.

## Phase 2: UAV Provider Adapter (Priority: P1)

**Independent Test**: A camera start creates one ready publisher, returns a key
only after safe join and Mapping readiness, and publishes zero plaintext or
cursor-named payload Data.

- [X] T002 [US1] After Spec 119 T004 passes, adapt `VideoPublisher` to `ServiceProvider::createLiveStream` in `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, and `tests/unit-tests/uav-protocol-state.t.cpp`: atomically allocate Stream/session/Mapping version/key/salt, measure at least three publication groups, find an SPS/PPS/IDR decoder-safe join, generate complete future semantic source/repair names, reserve them before production, encode and AES-GCM-protect each reservation-bound source VideoPacket, and pass only opaque envelopes to `publish` or Spec 119 `publishGroup` with `None`/`XorOneRepair`; remove UAV parity generation authority; return the key-bearing descriptor only after the handle reports registered routes, committed Mapping through join, and readiness; fail/wipe tentative secrets on no data, unsafe join, RNG, API, FEC wire cap, serialization, or capability failure; preserve idempotent equivalent start and secret-free stop/status/failure/ACK/unrelated responses (FR-001, FR-003 through FR-009, FR-011, FR-012, FR-015, FR-016, FR-018, FR-019, FR-021 through FR-025; SC-001, SC-003, SC-004, SC-007, SC-009).

## Phase 3: UAV Consumer Adapter (Priority: P1)

**Independent Test**: The ground station passes a strict descriptor to
`openLiveStream`, receives Provider-authenticated semantic-name opaque items,
accepts ordinary and `FecRecovered` bytes only after the same UAV AEAD/session/
replay gate, and feeds accepted plaintext VideoPackets into reorder/decoder.

- [X] T003 [US1] Replace the UAV ground station's legacy `streamPrefix/packetSeq`, independent pressure/window loop, and `FecFrameState` XOR recovery with one `ServiceUser::openLiveStream` handle in `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/configs/uav-stream-trust-schema.conf`, `NDNSF-UAV-APP/configs/uav_runtime.conf`, `NDNSF-UAV-APP/shared/UavNames.hpp`, `NDNSF-UAV-APP/tools/uav_deployment_check.py`, and `tests/unit-tests/uav-protocol-state.t.cpp`: strictly parse the key-bearing response against expected request/Provider/service, configure latest-mode and optional FEC from the validated descriptor, decrypt `SignedData` and `FecRecovered` opaque items only after Spec 119 Mapping/Provider/FEC admission, apply identical AAD/AEAD/session/replay checks, return accepted sample boundaries, preserve VideoPacket reorder/decoder/bitrate behavior, expose handle plus UAV decode diagnostics, and delete plaintext, cursor-name, manual Mapping, manual Face pump, duplicate controller, and duplicate XOR fallbacks (FR-008 through FR-012, FR-015, FR-017 through FR-025; SC-001, SC-002, SC-007, SC-009).

## Phase 4: Security And Lifecycle Closure (Priority: P1/P2)

- [X] T004 [US2] Close one adversarial and lifecycle gate in `tests/run_uav_stream_security_contract.py`, `tests/unit-tests/uav-protocol-state.t.cpp`, `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, and `tests/python/test_ndnsf_uav_stream_control_isolation_campaign.py`: cover missing permission/key/Mapping, wrong-valid-Provider, wrong root/session/version/range/continuity, equivocation/remap/stale cache, predicted-unproduced/late names, substituted name, nonce/replay/RNG/ciphertext/tag/plaintext failure, FEC off, one-loss recovery, corrupt/two-loss/wrong-signer/wrong-digest/expired FEC rejection, duplicate/post-eviction request, session replacement/restart/rekey, idempotent stop, forward-only revocation, secret/log scanning, and zero estimator/decrypt/decoder updates for rejection; prove all generic route/pending/fetch/controller/FEC behavior comes from Spec 119 handles, Core sees no key/plaintext, and old sessions accept no item (FR-004 through FR-025; SC-002 through SC-007, SC-009).

## Phase 5: MiniNDN Acceptance And Documentation

- [X] T005 [US3] Run exactly one 60-second protected UAV cell at 0% and 5% loss after focused and normal/Targeted regressions in `Experiments/NDNSF_UAV_Stream_Parity_Campaign.py`, `Experiments/NDNSF_UAV_GUI_Minindn.py`, `tests/run_uav_stream_security_contract.py`, `README.md`, `README_ch.md`, `docs/streaming-substrate.md`, and `specs/118-uav-stream-session-key/completion-summary.md`; require successful Spec 119 handles, protected descriptor, semantic payload/repair names from signed ahead Mapping, ciphertext-before-Core and decrypt-after-Core ordering, Provider/digest admission before recovered delivery, decoded-video/control evidence, bounded handle/APP/NFD PIT state, stop/start/rekey/stale rejection, zero plaintext/old-name/API/XOR duplication, and preserved negative FEC outcomes; distinguish implemented, wired, executed, measured, and deferred claims, then release Spec 119 T005 (FR-011 through FR-025; SC-001 through SC-009).

## Dependencies And Execution Order

```text
Spec119-T001/T002 -> T001
Spec119-T003/T004 + T001 -> T002 -> T003 -> T004 -> T005 -> Spec119-T005
```

No tasks are split by file, test, or evidence mechanics. Each task closes one
reviewable behavior: crypto contract, Provider adapter, Consumer adapter,
security/lifecycle, or integrated acceptance.

## Completion Rule

Spec 118 is complete only when the UAV uses the public Spec 119 API on both
sides, only successful protected start responses disclose keys, every video
payload keeps its semantic NDN name and is encrypted, invalid data reaches no
estimator/decrypt/decoder state, optional FEC is handle-owned over ciphertext,
no duplicate UAV XOR authority remains, lifecycle/secret gates pass, and the
two frozen MiniNDN cells preserve honest results.
