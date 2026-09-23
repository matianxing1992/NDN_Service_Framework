# Tasks: Core Streaming Substrate

**Input**: Design documents from `specs/057-core-streaming-substrate/`

**Prerequisites**: `spec.md`, `plan.md`

**Tests**: Required because this is a reusable core API.

## Phase 1: Setup

- [x] T001 Create `specs/057-core-streaming-substrate/` with spec, plan, and tasks.
- [x] T002 Confirm CodeGraph, GSD, and Spec Kit decision gates.

## Phase 2: Foundational Core API

- [x] T003 [P] Create `ndn-service-framework/Stream.hpp` and `Stream.cpp` with app-neutral C++ stream entities.
- [x] T004 [P] Implement C++ TLV encoding/decoding for stream info, chunks, and FEC metadata.
- [x] T005 [P] Add `pythonWrapper/ndnsf/streaming.py` as a thin orchestration/test mirror.
- [x] T006 Export streaming entities from `pythonWrapper/ndnsf/__init__.py`.

## Phase 3: Producer And Consumer Helpers

- [x] T007 Implement bounded `StreamProducerBuffer`.
- [x] T008 Implement `StreamConsumerReorderBuffer` with current-session, duplicate, and in-order emission behavior.
- [x] T009 Implement codec-neutral `StreamFecInfo` metadata helpers.

## Phase 4: Adaptive Fetch State

- [x] T010 Implement `StreamAdaptiveFetcherState` and `StreamFetchDecision`.
- [x] T011 Add policy helper for window, lookahead, interest lifetime, and missing timeout decisions.

## Phase 5: Tests

- [x] T012 [P] Add `tests/unit-tests/stream.t.cpp` and `tests/python/test_ndnsf_core_streaming.py` for metadata round trips.
- [x] T013 [P] Add C++/Python tests for producer buffer eviction and lookup.
- [x] T014 [P] Add C++/Python tests for reorder, duplicate, stale-session, and missing-gap behavior.
- [x] T015 [P] Add C++/Python tests for adaptive fetch decisions.

## Phase 6: Documentation

- [x] T016 Add `specs/057-core-streaming-substrate/streaming-substrate.md` with NDNSF/UAV boundary and migration mapping.
- [x] T017 Update `pythonWrapper/README.md` and `pythonWrapper/README_ch.md` with a short API entry.
- [x] T018 Update `NDNSF-UAV-APP/README.md` and `NDNSF-UAV-APP/README_ch.md` to reference the core substrate boundary.

## Phase 7: Verification

- [x] T019 Run C++ core streaming tests.
- [x] T020 Run Python streaming and existing core coordination tests.
- [x] T021 Review the implementation checklist against final implementation and record accepted/rejected suggestions.

## Phase 8: UAV Compatibility Mapping

- [x] T022 Add `VideoPacket` to core `StreamChunk` conversion helpers in `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T023 Add UAV protocol test proving conversion preserves stream/session/frame/FEC fields.
- [x] T024 Add UAV protocol test proving `encodeVideoPacket(...)` output is unchanged after `VideoPacket -> StreamChunk -> VideoPacket`.
- [x] T025 Run `UavProtocolState`, `Stream`, Python streaming tests, and full `./waf build`.

## Phase 9: UAV Producer Migration

- [x] T026 Update drone `publishCurrentFrame()` to construct core `StreamChunk` values for data shards.
- [x] T027 Update drone `publishCurrentFrame()` to construct core `StreamChunk` values for parity shards.
- [x] T028 Keep final publication on the existing `streamChunkToVideoPacket(...) -> encodeVideoPacket(...)` compatibility path.
- [x] T029 Run `UavProtocolState`, `Stream`, Python streaming tests, and full `./waf build` after the producer migration.

## Phase 10: UAV Consumer Handoff Migration

- [x] T030 Rename the ground-station decoder queue item to `DecoderStreamChunk` so it no longer collides with core `StreamChunk`.
- [x] T031 Add `insertStreamChunkForDecode(...)` as the single core `StreamChunk` handoff into the existing decoder queue.
- [x] T032 Convert non-FEC, FEC data-shard, and recovered FEC output paths to enter the decoder through core `StreamChunk`.
- [x] T033 Keep existing FEC recovery and decoder queue ordering intact; do not place core duplicate/reorder filtering before FEC.
- [x] T034 Run `UavProtocolState`, `Stream`, Python streaming tests, and full `./waf build` after the consumer handoff migration.

## Phase 11: Receiver-Side FEC Handoff Evidence

- [x] T035 Add a pure C++ receiver-side FEC handoff test using `VideoPacket -> StreamChunk` mapping.
- [x] T036 Verify data and parity `StreamChunk` values preserve enough metadata and payload to recover a missing data shard.
- [x] T037 Run `UavProtocolState`, `Stream`, Python streaming tests, and full `./waf build` after adding the handoff evidence.

## Phase 12: MiniNDN UAV Live-Stream Smoke

- [x] T038 Run the UAV MiniNDN quick smoke for binary/config/topology readiness.
- [x] T039 Run headless-drone auto video smoke through `Experiments/NDNSF_UAV_GUI_Minindn.py`.
- [x] T040 Verify the live stream starts, produces drone-owned video Data, decodes frames at the ground station, and stops cleanly.

## Phase 13: Durable Documentation

- [x] T041 Move the streaming substrate boundary and live-smoke recipe into tracked feature documentation.
- [x] T042 Keep ignored `docs/` output out of commits while preserving the evidence in `specs/057-core-streaming-substrate/streaming-substrate.md`.

## Phase 14: Lossy MiniNDN Stream Smoke

- [x] T043 Run the UAV MiniNDN quick smoke on `Experiments/Topology/UAV(loss=5%)`.
- [x] T044 Run headless-drone auto video smoke on the 5% loss topology.
- [x] T045 Record decoded-frame, packet, FEC-group, and clean-stop evidence for the 5% loss run.

## Phase 15: Non-Video DI Stream Smoke

- [x] T046 Update the LLM pipeline smoke dependency store to publish hidden-state bundles as core `StreamChunk` wires.
- [x] T047 Validate dependency metadata, content type, sequence, and segment count when reassembling DI tensor bundles.
- [x] T048 Run the LLM pipeline smoke with default and forced-small stream chunks to prove multi-chunk non-video reassembly.

## Phase 16: C++ DI Large-Data StreamChunk Path

- [x] T049 Add optional `StreamChunk` tensor-bundle wrapping to `NdnsfCollaborationDependencyIo`.
- [x] T050 Add `NativeProviderHandlerConfig::streamChunkDependencies` and `NDNSF_DI_STREAM_CHUNK_DEPENDENCIES=1` runtime enablement.
- [x] T051 Add unit tests for tensor-bundle StreamChunk round trip and invalid content/segment rejection.
- [x] T052 Run focused stream tests, Python streaming tests, and full C++ `unit-tests`.

## Phase 17: Real MiniNDN DI StreamChunk Correctness

- [x] T053 Locate the current MiniNDN Qwen/NativeTracer full-network entrypoint in `Experiments/` and record its raw-mode command in `specs/057-core-streaming-substrate/streaming-substrate.md`.
- [x] T054 Add or confirm a single flag/env hook in the MiniNDN harness under `Experiments/` that enables `NDNSF_DI_STREAM_CHUNK_DEPENDENCIES=1` for every provider process.
- [x] T055 Add a run-directory marker in the MiniNDN harness summary writer under `Experiments/` that records `dependency_envelope_mode=raw|streamchunk` in `config.json` or `summary.json`, while preserving the old `dependency_payload_mode` key as a compatibility alias.
- [x] T056 Run the existing raw-mode full-network smoke from `Experiments/` and save the result directory under `results/` as baseline evidence.
- [x] T057 Run the same full-network smoke from `Experiments/` with StreamChunk dependencies enabled and save the result directory under `results/`.
- [x] T058 Compare raw and StreamChunk final outputs using exact text or output hash in a script under `Experiments/` or `tools/`.
- [x] T059 Verify provider logs under the two `results/` directories show `publishLargeNamed/fetchLarge` dependency execution, not an in-memory store.
- [x] T060 Record the correctness result, commands, and accepted result paths in `specs/057-core-streaming-substrate/streaming-substrate.md`.

## Phase 18: C++ Stream Dependency Diagnostics

- [x] T061 Add a lightweight `NDNSF_DI_STREAM_DEPENDENCY` log line in `NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp` for publish/fetch success.
- [x] T062 Include session, scope, planned name, mode, payload bytes, wire bytes, envelope bytes, and status in the `NdnsfCollaborationDependencyIo.cpp` log line.
- [x] T063 Add decode-error logging in `NdnsfCollaborationDependencyIo.cpp` before rethrowing invalid StreamChunk dependency payloads.
- [x] T064 Add unit tests or focused parser tests in `tests/unit-tests/distributed-inference-async-runtime.t.cpp` or `tests/python/` for the counter/summary extraction format if a parser is introduced.
- [x] T065 Re-run `./build/unit-tests --run_test=NdnsfCollaborationDependencyIoWrapsTensorBundleAsStreamChunk`, `./build/unit-tests --run_test=Stream`, `tests/python/test_ndnsf_core_streaming.py`, and full `./build/unit-tests`.

## Phase 19: Raw vs StreamChunk Overhead Campaign

- [x] T066 Create or extend a small campaign script under `Experiments/` that runs the same MiniNDN DI workload in raw mode and StreamChunk mode.
- [x] T067 Collect p50/p95/p99 latency, dependency fetch p50/p95, request completion count, failure rate, timeout count, and output hash into `summary.json` under each `results/` run directory.
- [x] T068 Collect raw payload bytes, StreamChunk wire bytes, envelope bytes, and overhead ratio into `summary.json` or `streamchunk_counters.json` under each `results/` run directory.
- [x] T069 Run at least 3 repetitions per mode from the `Experiments/` campaign script for smoke; use 10 repetitions if runtime is acceptable.
- [x] T070 Generate a compact CSV/table under the accepted `results/` campaign directory comparing both modes.
- [x] T071 Record the accepted benchmark command and result path in `specs/057-core-streaming-substrate/streaming-substrate.md`.

## Phase 20: Loss And Robustness Smoke

- [x] T072 After 0% loss is stable, run raw and StreamChunk modes from `Experiments/` on a low-loss MiniNDN topology, preferably 1-5%.
- [x] T073 Confirm from `results/` logs that StreamChunk mode introduces no decode failures, hangs, or new timeout pattern versus raw mode.
- [x] T074 Record in `specs/057-core-streaming-substrate/streaming-substrate.md` whether loss behavior is unchanged, worse, or better; do not claim improvement unless the data shows it.

## Phase 21: GUI And Headless Experiment Entry

- [x] T075 Expose the dependency payload mode in the NDNSF-DI GUI/headless config path under `Experiments/` or the GUI module that launches DI processes.
- [x] T076 Ensure headless and non-headless GUI execution under `Experiments/` pass the same dependency payload mode into provider processes.
- [x] T077 Add a headless smoke under `Experiments/` or `tests/python/` that runs StreamChunk mode without manual GUI interaction.
- [x] T078 Show the dependency payload mode and summary counters in GUI status text or run summary output under `Experiments/` or the GUI module.

## Phase 22: Documentation And Default Decision

- [x] T079 Document when to use raw dependency mode versus StreamChunk dependency mode in `specs/057-core-streaming-substrate/streaming-substrate.md`.
- [x] T080 Add a troubleshooting section in `specs/057-core-streaming-substrate/streaming-substrate.md` for content-type mismatch, segment mismatch, scope/session mismatch, and timeout.
- [x] T081 Decide in `specs/057-core-streaming-substrate/streaming-substrate.md` whether StreamChunk dependency mode remains opt-in or becomes the default, based only on Phase 17-20 evidence.
- [x] T082 If default changes, add an explicit opt-out path in `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` or the relevant `Experiments/` harness and re-run all required tests in both modes. Default did not change, so the existing opt-in flag remains the explicit path.
