# Tasks: High-Availability Concurrent Distributed Repo

**Input**: Design documents from `specs/077-repo-ha-concurrency/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, contracts, and `experiment-plan.md`

**Execution Rule**: Work phase by phase. Write focused tests before each implementation group, run compatibility tests after each phase, and update this checklist immediately after verification.

## Phase 1: Context and Baseline

- [x] T001 Read the constitution, active Repo Specs 073-076, and current dirty-worktree state.
- [x] T002 Run CodeGraph status/exploration for RepoCore, RepoNodeApp, clients, producers, catalog, repair, and placement paths.
- [x] T003 Use the ARS experiment workflow to define variables, controls, metrics, duration, and failure injection in `experiment-plan.md`.
- [x] T004 Validate GSD health and create Phase 9 resumable planning artifacts under `.planning/phases/09-repo-ha-concurrency/`.
- [x] T005 Record current focused Python baseline: exact packets, tiered cache, and core discovery.
- [x] T006 Record the current C++ Repo build/test baseline before implementation.
- [x] T007 Document external design decisions and intentional non-goals in `research.md`.
- [x] T008 Update `.specify/feature.json`, agent context, ROADMAP, and STATE to Spec 077/Phase 9.

## Phase 2: Shared Contracts and Schema

- [x] T009 [P] Add write consistency/state enums and normalized string conversions to C++ `RepoTypes`.
- [x] T010 [P] Extend C++ `RepoObjectManifest` with generation, parent generation, required acknowledgements, confirmed replicas, operation ID, and lifecycle state using backward-compatible JSON defaults.
- [x] T011 [P] Add C++ `RepoWriteIntent`, `RepoWriteReceipt`, `RepoCapacityReservation`, and extended operation-status types.
- [x] T012 Add equivalent Python dataclasses/serialization and reject invalid state/acknowledgement combinations.
- [x] T013 Add SQLite schema migration for repo metadata, operations, receipts, serving packets/prefixes, catalog journal/tombstones, peer watermarks, membership, repair jobs, and reservations.
- [x] T014 Add persisted schema version and idempotent migration tests from the Spec 073-076 database shape.
- [x] T015 Configure SQLite WAL, explicit transaction behavior, foreign keys, and bounded `busy_timeout` on writer/read connections.
- [x] T016 Add operation/rejection reason constants shared by ACK, response, repair, and diagnostics paths.
- [x] T017 Add protocol contract tests for legacy manifest decoding and new manifest/receipt round trips.
- [x] T018 Run C++ and Python contract/schema tests and fix compatibility regressions.

## Phase 3: Confirmed and Idempotent Writes

- [x] T019 Write failing tests for idempotent operation replay, operation-content conflict, receipt persistence, ALL/QUORUM/ONE thresholds, partial write, and CAS conflict.
- [x] T020 Implement replica-side transactional write-intent lookup/create and operation-content conflict rejection.
- [x] T021 Implement durable receipt creation in the same SQLite transaction as object/packet persistence.
- [x] T022 Return structured write receipts from STORE, STORE_MANIFEST, STORE_PACKETS, STORE_PACKET_BATCH, and STORE_PACKET_PULL.
- [x] T023 Implement expected-generation validation and generation advancement for mutable object/manifest APIs.
- [x] T024 Preserve exact Data same-name/same-wire idempotency and same-name/different-wire rejection.
- [x] T025 Replace ACK-selected-replica assumptions with validated per-replica receipt collection in NetworkDistributedRepoClient.
- [x] T026 Build committed manifests from confirmed replicas only and return structured incomplete-write evidence below W.
- [x] T027 Persist partial replica evidence and make retry with the same operation ID resume safely.
- [x] T028 Run confirmed-write tests plus Specs 073-076 regressions.

## Phase 4: Always-On Repo Data Plane

- [x] T029 Write failing native/Python tests proving producer thread count does not grow with fetch count and exact wires remain identical.
- [x] T030 Implement pybind `RepoDataPlaneProducer` with one Face/event thread, dynamic prefix activation, stable route registration, and callback lookup.
- [x] T031 Add safe callback/GIL/error handling and validate returned Data wire/name against the Interest.
- [x] T032 Add Python wrapper API for starting, activating prefixes, querying counters/errors, and stopping the data plane.
- [x] T033 Implement exact packet lookup by Interest name through bounded cache then thread-local SQLite reader.
- [x] T034 Persist serving-prefix activation and restore it during Repo startup.
- [x] T035 Change exact packet FETCH_PREPARE to activate the long-lived data plane instead of creating StoredDataProducer/timer instances.
- [x] T036 Persist deterministic serving packets for opaque objects on first preparation and serve later fetches through the same data plane.
- [x] T037 Advertise only the stable Repo serving route through NLSR and remove per-fetch/per-object external route advertisements from the scalable path.
- [x] T038 Run producer-bound, restart, exact-wire, opaque SegmentFetcher, and Specs 073-076 compatibility tests.

## Phase 5: Concurrent Storage and Runtime Metrics

- [x] T039 Write failing tests for parallel unrelated reads, read/write same-object coherence, bounded writer admission, incremental capacity, and queue metrics.
- [x] T040 Split the Python storage lock into writer, metadata/stats, and striped per-object locks.
- [x] T041 Add thread-local SQLite read connections and route read-only object/manifest/packet/inventory queries through them.
- [x] T042 Maintain persisted/in-memory used-byte counters transactionally instead of full-table SUM on each capability ACK.
- [x] T043 Add bounded foreground write/read admission and explicit overload rejection before expensive work.
- [x] T044 Track inflight reads/writes/repair, queue depth, rejection count, and storage latency EWMA/histogram snapshots.
- [x] T045 Publish live runtime metrics in typed ProviderCapabilityHint and legacy ACK fields with timestamps.
- [x] T046 Refactor C++ RepoCore/TieredRepoStore lock scope so unrelated reads do not hold a RepoCore mutex across backing I/O.
- [x] T047 Add bounded operation-status retention/cleanup in C++ and Python.
- [x] T048 Run concurrency, cache coherence, C++ Repo, and existing Python regressions.

## Phase 6: Durable Catalog, Membership, Anti-Entropy, and Repair

- [x] T049 Write failing tests for restart-persistent journal/tombstone/watermark/jobs, empty-delta heartbeat, stale-node repair, repair recurrence, and conflict detection.
- [x] T050 Generate/persist Repo boot incarnation and monotonic local catalog sequence.
- [x] T051 Persist local and merged catalog journal entries and reconstruct global catalog state on restart.
- [x] T052 Persist tombstones and order entries by source incarnation/sequence rather than wall-clock alone.
- [x] T053 Persist peer watermarks and membership heartbeat snapshots even when a delta contains zero entries.
- [x] T054 Bound catalog delta history and implement safe snapshot/watermark compaction.
- [x] T055 Implement deterministic bucket digest and bucket-entry operations for lightweight anti-entropy.
- [x] T056 Detect same-generation/different-digest live replicas and expose CONFLICT without arbitrary best-entry selection.
- [x] T057 Implement durable idempotent repair-job creation from periodic under-replication scans.
- [x] T058 Implement repair leases, retries/backoff, completion/failure updates, and lease-expiry recovery.
- [x] T059 Update catalog sidecar to scan every interval, consume durable jobs, and remove process-lifetime executed-action suppression.
- [x] T060 Ensure a later target loss creates a new repair epoch/job after an earlier successful repair.
- [x] T061 Limit repair concurrency/admission below foreground reads and writes.
- [x] T062 Add scrub/integrity scan counters and a bounded operation that verifies manifest/wire digests.
- [x] T063 Run durable catalog/repair/anti-entropy tests and restart regressions.

## Phase 7: Placement, Reservations, and Fast Read Failover

- [x] T064 Write failing tests for placement TTL, failure invalidation, reservation oversubscription, telemetry scoring, total-deadline failover, and health cooldown.
- [x] T065 Add placement cache timestamps/TTL and invalidate cached nodes after timeout, rejection, integrity failure, or capacity failure.
- [x] T066 Incorporate live queue/inflight/storage latency and available network RTT/bandwidth into placement scoring while retaining failure-domain diversity.
- [x] T067 Implement durable expiring capacity reservations and subtract active reservations from advertised free bytes.
- [x] T068 Consume/release reservations transactionally with write receipt success/failure and expire abandoned reservations.
- [x] T069 Add per-replica client health EWMA, failure cooldown, and health-aware replica ordering.
- [x] T070 Divide one total read deadline into bounded per-replica attempts and fast-fail on Nack, timeout, or invalid wire.
- [x] T071 Preserve whole-packet-set restart semantics while reducing dead-primary wait.
- [x] T072 Add optional delayed hedged read for opaque objects behind an explicit configuration flag; never mix packet sets.
- [x] T073 Run placement/reservation/failover tests and compare failover latency with the Spec 076 baseline.

## Phase 8: Security, APIs, and Documentation

- [x] T074 Enforce server-side publisher ownership for mutable object names using authenticated request identity available from NDNSF.
- [x] T075 Add configurable exact-Data validation policy metadata without rewriting or re-signing stored wires.
- [x] T076 Expose new consistency, generation, status, reservation, metrics, scrub, and repair operations through C++/Python APIs and examples.
- [x] T077 Keep English and Chinese NDNSF-DistributedRepo README documentation synchronized.
- [x] T078 Document the canonical C++ contract/Python orchestration boundary and prohibit new duplicate policy implementations.
- [x] T079 Document standard Repo async-status/direct-read alignment and explicitly isolate future repo-ng command-wire compatibility.
- [x] T080 Run security-focused and API compatibility regressions.

## Phase 9: MiniNDN Campaign and Evidence

- [x] T081 Build a headless workload driver supporting exact/opaque objects, read/write/mixed ratios, concurrency, RF/W, deterministic seeds, and sampled lifecycle output.
- [x] T082 Extend the MiniNDN topology with 3-5 Repo nodes in distinct failure domains and configurable storage/runtime profiles.
- [x] T083 Add controlled node termination/restart during write, read, and repair phases.
- [x] T084 Add aggregation for stable RPS, p50/p95/p99, failure/rejection, receipts, failover, repair, cache, storage latency, and resource bounds.
- [x] T085 Run a short smoke campaign and fix functional/network integration defects.
- [x] T086 Run 60-second read-heavy sweeps at concurrency 1, 4, 16, and 32.
- [x] T087 Run 60-second mixed and write-heavy sweeps at the stable concurrency envelope.
- [x] T088 Run node-loss campaigns during write, read, and repair.
- [x] T089 Run restart/tombstone/catalog/serving-prefix recovery campaign.
- [x] T090 Compare measured failover with Spec 076 and report improvement or negative result honestly.
- [x] T091 Preserve canonical machine-readable results and remove superseded debug attempts.
- [x] T092 Update quickstart and experiment documentation with exact commands and result paths.

## Phase 10: Convergence and Acceptance

- [x] T093 Run full C++ build and all Repo C++ test executables.
- [x] T094 Run all focused Python Repo tests and relevant shared runtime/security regressions.
- [x] T095 Run Spec Kit consistency analysis and requirement checklist audit.
- [x] T096 Run GSD code review, verification, and health checks; record residual risks.
- [x] T097 Run CodeGraph impact review for orphaned legacy producer/catalog paths and remove only proven-unused Repo-specific code.
- [x] T098 Mark every requirement/task with evidence and set Spec 077, GSD Phase 9, ROADMAP, and STATE to complete only if all gates pass.
- [x] T099 Play completion bell and report changed files, tests, performance, node-loss evidence, workflow gates, and the next highest-value project step.

## Dependencies

- Phase 2 blocks every implementation phase.
- Confirmed writes (Phase 3) block repair receipt verification and failure campaigns.
- Always-on data plane (Phase 4) and storage concurrency (Phase 5) block performance claims.
- Durable catalog (Phase 6) blocks node-loss repair claims.
- Placement/reservations (Phase 7) depend on live metrics from Phase 5.
- MiniNDN campaign begins only after Phases 3-8 focused tests pass.
- Completion requires all Phase 10 gates; passing unit tests alone is insufficient.

## Parallel Opportunities

- C++ contract types and Python schema tests can proceed independently after T008.
- Native data-plane wrapper and Python catalog persistence touch separate areas after contract fields stabilize.
- Experiment driver/aggregator can be built while focused implementation tests run, but campaigns wait for integration.
- Documentation may be updated alongside tested APIs, with final synchronization in Phase 10.
