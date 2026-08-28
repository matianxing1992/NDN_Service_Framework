# T003 Persistence Authority Evidence

Date: 2026-07-29

## Delivered boundary

- `PayloadStore` and `MetadataStore` native contracts are separated behind one
  `RepositoryStoreFacade`.
- Python and C++ use the same canonical
  `<database>.authority.lock` convention. A second live authority fails closed
  with `repo-persistence-owned`.
- Existing Python SQLite calls receive an owned connection view whose
  `commit`, `rollback`, and `close` operations return to the facade.
- The metadata schema is generation 9 and contains a durable lifecycle journal.
- Lifecycle transitions are bounded, strictly ordered, identity-bound,
  idempotent by event ID, and auditable. Rejected state, identity, and
  transition attempts are recorded but do not advance authoritative state.
- The legacy `exact-packet-v1` SQLite payload path remains explicit; the
  scalable filesystem CAS is intentionally deferred to T006.

## Verification

```text
./waf build --targets=unit-tests,ndnsf-distributed-repo -j2
PASS

./build/unit-tests --run_test=DistributedRepoStoreBackend --log_level=test_suite
PASS (2/2)

python3 setup.py build_ext --inplace
PASS

python3 tests/python/test_spec164_persistence_authority.py -v
PASS (6/6)

python3 tests/python/test_ndnsf_repo_exact_packets.py -v
PASS (12/12)

python3 tests/python/test_ndnsf_repo_tiered_cache.py -v
PASS (11/11)

python3 tests/python/test_ndnsf_repo_ha.py -v
PASS (48/48)

python3 tests/python/test_spec164_legacy_subject.py -v
PASS (3/3)

python3 tests/python/test_ndnsf_repo_chunked_file.py -v
PASS (2/2)

python3 tests/python/test_spec164_artifact_types.py -v
PASS (8/8)
```

All Python commands used:

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper
```

## Preserved negative evidence

- The first native test link failed because the existing unit-test target did
  not include `RepoTypes.cpp`; adding the production source exposed its
  `RepoProtocol.cpp` dependency. Both sources are now part of the real test
  target and the same target passes.
- Existing test fixtures constructed `RepoNodeApp` with `__new__` and bypassed
  the constructor. `_init_sqlite()` now adopts those connections into the
  authority facade instead of introducing a test-only bypass.
- The T001 test previously asserted that current source bytes could never
  diverge from the frozen baseline. T003 necessarily changes orchestration, so
  the frozen subject now has an independent canonical integrity digest while
  current compatibility is verified by the exact-packet, cache, HA, and
  chunked-file regressions. The frozen subject and `NOT_MEASURED` verdict were
  not rewritten.

No MiniNDN, TigerCluster, network-performance, or large-model experiment was
run for this persistence-contract task.

## Workflow gates

- Context Mode: `ctx_stats` ran, but the repository guard still reported zero
  project ContentDBs; all authority came from repository documents and current
  source instead of session recall.
- CodeGraph: traced the current native/Python persistence paths before edits,
  then synchronized and reread the new native facade.
- Spec Kit: prerequisites resolved Spec 164 and its required documents;
  requirements checklist passed 16/16.
- GSD: installation health validation passed; its only informational item was
  an unrelated in-progress phase without a summary.
- ARS: not invoked because T003 is implementation-only and does not change the
  already-designed research experiment or make a literature claim.
