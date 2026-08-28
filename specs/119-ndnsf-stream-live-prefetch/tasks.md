---
description: "Cohesive tasks for the generic NDNSF LiveStream prefetch API"
---

# Tasks: NDNSF Stream Live Prefetch

## Architecture Boundary

Spec 119 owns the complete reusable NDNSF network API. Payload Data keeps its
application semantic name; only signed Mapping block names are predictable and
sequential. Content is opaque: applications encrypt before publication and
decrypt after callback admission. Spec 119 also owns default-off bounded XOR
recovery over opaque bytes. Spec 118 consumes this API for encrypted UAV video
and MUST NOT reimplement Mapping routes, Face scheduling, controller policy,
producer pending state, or generic FEC.

## Phase 1: Frozen Mapping And Controller Foundation

- [X] T001 [US1] Freeze and implement the app-neutral `StreamCursor`/`StreamNameMapBlock` wire, predictable typed Mapping names, canonical Content digest chain, fixed-capacity cursor-to-original-name/tombstone resolver, five frontiers, terminal-unproduced handling, bounded caches, malformed/conflict/session/retention rejection, and byte-identical C++/Python golden vectors in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, `tests/fixtures/stream-prefetch/`, `tests/unit-tests/stream.t.cpp`, and `tests/python/test_ndnsf_core_streaming.py` (FR-001 through FR-003, FR-014 through FR-016, FR-019 through FR-026; SC-001, SC-002, SC-004, SC-008).

- [X] T002 [US1] Extend the single existing `StreamAdaptiveFetcherState` into the configured Inactive/Chasing/Adjusting/Fetching/Recovering/Stopped controller with paper-literal Eq. (3) reproduction kept separate from NDNSF profiles, measured sample period, retrieval-delay demand, bounded burst/withhold hysteresis, one Mapping/payload/retransmission budget, Congestion Nack/Mark decrease-and-hold, future-wait versus loss classification, recovery budget, diagnostics, and C++/Python parity using shared deterministic traces in the T001 owners (FR-004 through FR-012, FR-015, FR-017, FR-018, FR-020, FR-022, FR-023; SC-001, SC-003, SC-008).

**Checkpoint**: The pure codec, resolver, and decision engine are frozen and
tested, but no application should yet implement its own network loop.

## Phase 2: Simple Public LiveStream API (Priority: P1)

**Goal**: An app-neutral producer and consumer can use meaningful original Data
names with future exact-name prefetch and optional FEC through one lifecycle API.

**Independent Test**: A generic binary-payload example reserves semantic names,
publishes Mapping before payload, opens latest and beginning consumers, receives
only Provider-authenticated original-name opaque bytes, recovers one missing
opaque item when XOR is enabled, and stops without UAV, crypto, or manual
Face/Mapping code.

- [X] T003 [US2] Freeze the public C++/Python contract in `specs/119-ndnsf-stream-live-prefetch/contracts/live-stream-api.md`, then implement `LiveStreamDefinition`, immutable item/group reservations, `LiveStreamFecOptions` (`None` and bounded `XorOneRepair`), Provider-signed/digest-bound FEC metadata, `LiveStreamReadiness`, `LiveStreamPublisher`, provenance-bearing `VerifiedLiveStreamItem`, `LiveStreamItemAdmission`, `LiveStreamStatus`, and `LiveStreamConsumerHandle` through `ServiceProvider::createLiveStream(...)` and `ServiceUser::openLiveStream(...)` in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `ndn-service-framework/ServiceProvider.hpp`, `ndn-service-framework/ServiceProvider.cpp`, `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceUser.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, and `pythonWrapper/ndnsf/streaming.py`; use the existing Face/event loop and validator, keep publishers in `Preparing` until atomic activation, make `reserveAhead`/`reserveGroup` seal semantic Mapping before materialize-once `publish`/`publishGroup`, treat every supplied source as opaque, expose no key/cipher/encrypt/decrypt surface, validate FEC Provider/group/length/digest before local recovered delivery, never cache/republish recovered bytes as original Data, bound all Mapping/FEC/retention/pending state, compose T002, expose `start/status/stop` plus asynchronous accepted-sample observation, and ensure rejected/decrypt-invalid items update no controller state (FR-013 through FR-031; SC-001 through SC-004, SC-008, SC-009).

- [X] T004 [US2] Prove the public API end to end with test-first app-neutral C++ and Python clients plus one MiniNDN regression in `examples/LiveStreamProvider.cpp`, `examples/LiveStreamConsumer.cpp`, `examples/python/live_stream/`, `Experiments/NDNSF_LiveStream_Minindn.py`, `tests/unit-tests/stream.t.cpp`, `tests/python/test_ndnsf_core_streaming.py`, and `tests/python/test_ndnsf_live_stream_minindn.py`: cover preparing/atomic activation, latest/beginning start, one-item and asynchronous multi-item observation, Mapping-ahead future Interests, cache reuse, multiple consumers, late Mapping, timeout/Nack/congestion, FEC disabled, every one-source-loss XOR position with byte-identical opaque recovery, and wrong signer/group/digest/length, corrupt repair, two-loss, oversize and expired recovery rejection; prove no API/diagnostic contains a key or invokes crypto, no recovered value is cached/republished, invalid application admission updates no estimator, state is bounded, stop/replacement is idempotent, and payloads are neither cursor-named nor nested Data; preserve exact commands/evidence in `specs/119-ndnsf-stream-live-prefetch/quickstart.md` (FR-001 through FR-031; SC-001 through SC-005, SC-008, SC-009).

**Checkpoint**: Spec 119 is independently useful. Any NDNSF application can
publish and consume an original-name live stream without UAV code.

## Phase 3: UAV Adoption And Measured Decision (Priority: P2)

- [X] T005 [US3] After Spec 118 completes its UAV API adapter and security gate, run the predeclared matched MiniNDN campaign in `Experiments/NDNSF_UAV_Stream_Parity_Campaign.py` and `tests/python/test_ndnsf_uav_stream_parity_campaign.py`: compare pressure rollback, mapped live future-on, and the same Core controller with future lookahead disabled; use at least five fresh-process 60-second repetitions at 0% and 5% loss with recorded order seed, matched topology/source/control traces, nonzero eligible denominators, Provider Interest-before-production and Mapping-ready gates, bounded APP/NFD PIT state, security/completion gates, local-clock event correlation, and the frozen 4-of-5 plus 10%/5% adoption rule; report Mapping bytes and Interest share directly, preserve negative Mapping-cost or future-wait results without tuning them away, and synchronize `README.md`, `README_ch.md`, `docs/streaming-substrate.md`, and `specs/119-ndnsf-stream-live-prefetch/completion-summary.md` (SC-005 through SC-008).

## Dependencies And Execution Order

```text
T001 -> T002 -> T003 -> T004 -> Spec118-T002..T005 -> T005
```

- T001/T002 are complete foundations.
- T003 is the blocking public API; Spec 118 may not own a substitute network loop.
- T004 proves the API without UAV assumptions before security adaptation.
- Spec 118 then supplies UAV naming, encryption/decryption, decoder, and UI
  callbacks while selecting the generic optional FEC setting.
- T005 is the only multi-repetition performance/adoption campaign.

No task is split into mechanical test/implementation/evidence fragments. Each
task is one independently reviewable behavior with one acceptance gate.

## Completion Rule

Spec 119 is complete only when the public API exists in C++ and Python, the
app-neutral MiniNDN regression proves future exact-name prefetch over semantic
payload names and optional opaque-byte recovery, Spec 118 uses the API rather
than duplicating it, security and bounded-state gates pass, and the measured
campaign honestly selects or rejects the candidate default.
