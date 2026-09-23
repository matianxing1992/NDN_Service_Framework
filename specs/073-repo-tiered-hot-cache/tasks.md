# Tasks: SQLite-Authoritative Repo Hot Cache

## Phase 1: Setup and Durable Design

- [x] T001 Audit C++/Python Repo storage, protocol, client, CLI, test, and MiniNDN paths with CodeGraph and record findings in `specs/073-repo-tiered-hot-cache/plan.md`
- [x] T002 Define authority, cache, compatibility, and acceptance requirements in `specs/073-repo-tiered-hot-cache/spec.md`
- [x] T003 [P] Record storage decisions and second-pass findings in `specs/073-repo-tiered-hot-cache/research.md`
- [x] T004 [P] Define cache status and entry models in `specs/073-repo-tiered-hot-cache/data-model.md`
- [x] T005 [P] Define the `CACHE_STATUS` wire contract in `specs/073-repo-tiered-hot-cache/contracts/cache-status.md`
- [x] T006 [P] Define deterministic MiniNDN variables and acceptance evidence in `specs/073-repo-tiered-hot-cache/experiment-plan.md`

## Phase 2: Foundational Contracts

- [x] T007 Add `RepoCacheStatus`, backend status hooks, and tiered-store factories in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`
- [x] T008 [P] Add cache-status JSON parsing declaration in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`
- [x] T009 [P] Add core/node/client cache-status API declarations in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`, `RepoNode.hpp`, and `RepoClient.hpp`

## Phase 3: Durable SQLite Authority (User Story 1)

**Independent Test**: A committed object survives store destruction/recreation; injected backing-write failure never admits it.

- [x] T010 [US1] Implement authoritative-first tiered `put`, `putManifest`, `get`, `has`, `erase`, inventory, and usage delegation in `NDNSF-DistributedRepo/src/RepoTypes.cpp`
- [x] T011 [US1] Add restart, overwrite/delete, and failed-backing-write cases in `NDNSF-DistributedRepo/examples/DistributedRepoTieredCacheTest.cpp`
- [x] T012 [US1] Change persistent Python Repo writes/deletes to commit SQLite before cache/catalog publication in `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`
- [x] T013 [US1] Remove the unbounded persistent-mode `LocalDistributedRepo` payload duplicate and bound temporary producer retention in `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`
- [x] T014 [US1] Add Python restart, failure isolation, overwrite, delete, and persistent-authority tests in `tests/python/test_ndnsf_repo_tiered_cache.py`

## Phase 4: Bounded Hot Cache (User Story 2)

**Independent Test**: Known access order yields expected hit/miss/eviction counters and never exceeds one shared logical budget.

- [x] T015 [US2] Implement O(1) C++ byte-bounded LRU admission, recency, eviction, oversized bypass, and counters in `NDNSF-DistributedRepo/src/RepoTypes.cpp`
- [x] T016 [US2] Add C++ zero-budget, exact-budget, oversized, LRU, and concurrent-access cases in `NDNSF-DistributedRepo/examples/DistributedRepoTieredCacheTest.cpp`
- [x] T017 [US2] Implement one typed `_BoundedRepoHotCache` for object and packet entries in `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`
- [x] T018 [US2] Route Python read-through, write-through, packet, invalidation, and accounting paths through the unified cache in `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`
- [x] T019 [US2] Add Python shared-object/packet budget, LRU, zero-budget, oversized, and concurrency tests in `tests/python/test_ndnsf_repo_tiered_cache.py`

## Phase 5: Configuration and Observability (User Story 3)

**Independent Test**: Direct, embedded, remote C++, and Python operation paths return consistent cache status after a controlled sequence.

- [x] T020 [US3] Serialize `RepoCacheStatus` and parse its JSON in `NDNSF-DistributedRepo/src/RepoTypes.cpp` and `RepoProtocol.cpp`
- [x] T021 [US3] Wire cache status through C++ `RepoCore`, `RepoNode`, embedded registration, remote registration, and `RepoClient` in `NDNSF-DistributedRepo/src/RepoCore.cpp`, `RepoNode.cpp`, and `RepoClient.cpp`
- [x] T022 [US3] Require SQLite-backed operation and add `tiered`/`memory-cache-bytes` CLI/config handling and startup reporting in `NDNSF-DistributedRepo/apps/DistributedRepoNodeApp.cpp`
- [x] T023 [US3] Set the persistent sample to tiered mode in `NDNSF-DistributedRepo/configs/repo-node.conf`
- [x] T024 [US3] Add Python `CACHE_STATUS`, capability metadata, ACK metadata, and network-client access in `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`
- [x] T025 [US3] Exercise direct/embedded status and update build targets in `NDNSF-DistributedRepo/examples/DistributedRepoSmoke.cpp` and `NDNSF-DistributedRepo/wscript`
- [x] T026 [US3] Document SQLite-only authority, cache configuration, counters, and zero-budget behavior in `NDNSF-DistributedRepo/README.md`

## Phase 6: MiniNDN Network Acceptance (User Story 4)

**Independent Test**: One command produces a passing JSON summary for restart, cold miss, hot hit, eviction, and backing fallback over NDNSF.

- [x] T027 [US4] Add tiered-cache seed/verify client modes and JSON state handling in `examples/python/NDNSF-DistributedRepo/generic_object_store/client.py`
- [x] T028 [US4] Add Repo-node cache budget/producer-retention CLI plumbing in `examples/python/NDNSF-DistributedRepo/generic_object_store/repo_node.py`
- [x] T029 [US4] Add Repo A stop/restart orchestration, status assertions, and summary output in `Experiments/NDNSF_DistributedRepo_Generic_Minindn.py`
- [x] T030 [US4] Run the dedicated MiniNDN scenario and retain its canonical command, logs, and summary under `results/distributed_repo_tiered_cache_minindn/`

## Phase 7: Verification and Convergence

- [x] T031 Build `ndnsf-distributed-repo`, `DistributedRepoNodeApp`, `DistributedRepoSmoke`, and `DistributedRepoTieredCacheTest`
- [x] T032 Run C++ tiered-cache and existing DistributedRepo smoke executables
- [x] T033 Run focused Python tiered-cache, zero-budget SQLite, and existing Repo regression suites
- [x] T034 Run `git diff --check`, inspect CodeGraph blast radius, and verify no proposal slides or unrelated dirty files changed
- [x] T035 Reconcile every requirement/task with evidence, update `specs/073-repo-tiered-hot-cache/tasks.md`, and run Spec Kit analyze/converge checks
- [x] T036 Update `specs/073-repo-tiered-hot-cache/quickstart.md` and `NDNSF-DistributedRepo/README.md` with final verified commands/results

## Dependencies

```text
Phase 1 -> Phase 2 -> US1 -> US2 -> US3 -> US4 -> Verification
```

US1 establishes authority ordering. US2 depends on that ordering so cache behavior cannot create a second source of truth. US3 exposes the counters needed by US4. MiniNDN acceptance runs only after local regressions pass.

## Parallel Opportunities

- T008 and T009 can proceed after T007's field names are fixed.
- C++ tests and Python tests can be drafted in parallel with their respective implementations.
- Documentation and sample configuration can be updated while local test builds run.

## Implementation Strategy

The MVP is US1 plus one bounded object cache path. Complete packet-cache parity, remote observability, and MiniNDN restart/eviction evidence before declaring the feature done.

## Phase 8: Convergence

- [x] T037 Serialize Python authority/cache state transitions and add a deterministic concurrent read/overwrite regression per FR-004, FR-009, and FR-016 (partial)
- [x] T038 Replace the C++ tiered-cache tree lookup with an O(1) hash lookup per plan performance goals (partial)
- [x] T039 Align C++ zero-budget `storageBackend` with the `CACHE_STATUS` contract and Python parity per FR-014 (contradicts)
- [x] T040 Make post-commit Python cache admission failure non-fatal and add a committed-write regression per FR-002 and the cache-as-optimization design (partial)
