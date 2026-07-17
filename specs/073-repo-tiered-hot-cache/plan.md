# Implementation Plan: SQLite-Authoritative Repo Hot Cache

**Branch**: `Experimental` | **Date**: 2026-07-09 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/073-repo-tiered-hot-cache/spec.md`

## Summary

Make every deployed Repo node use SQLite as the sole source of truth and a single byte-bounded LRU memory tier for newly committed and repeatedly fetched content. Implement the contract in the C++ DistributedRepo core and in the Python network Repo used by current MiniNDN applications. Writes are write-through with commit-before-admission ordering; reads are read-through; oversized entries bypass RAM; delete/overwrite follow authoritative-first ordering. A `CACHE_STATUS` operation exposes configuration and counters through direct, embedded, and remote NDNSF paths. A dedicated MiniNDN flow proves restart persistence, miss-to-hit behavior, eviction, and SQLite fallback.

## Technical Context

**Language/Version**: C++17; Python 3.8+ on Ubuntu 20.04

**Primary Dependencies**: ndn-cxx, NDNSF dynamic runtime, SQLite3, Python `sqlite3`, MiniNDN, NFD/NLSR

**Storage**: SQLite WAL database as authority; process-local bounded LRU as disposable acceleration

**Testing**: C++ smoke/test executables, Python `unittest`, existing NDNSF regressions, dedicated MiniNDN functional scenario

**Target Platform**: Linux NDNSF Repo nodes, including Ubuntu 20.04 MiniNDN environments

**Project Type**: Shared C++ library plus node CLI, Python network application adapter, examples, and experiments

**Performance Goals**: Repeated reads avoid SQLite after admission; cache bookkeeping is O(1); no cache operation scans repository contents

**Constraints**: Memory usage is bounded by a deterministic logical charge; SQLite commits precede visibility; deployed Repo nodes never fall back to memory-only authority; existing wire APIs remain compatible; no NDNSF security bypass or proposal-slide changes

**Scale/Scope**: One cache per Repo process; opaque object payloads and whole segmented-packet sets share one budget; active transfer producers are temporary serving resources rather than durable cache entries

## Constitution Check

*GATE: Passed before design and re-checked after Phase 1.*

- **Canonical Dynamic Runtime**: `CACHE_STATUS` uses the existing generic Request/Response service registration and unified Repo service naming.
- **Security Is Part Of The Data Path**: No permission, NAC-ABE, token, or trust-schema behavior changes.
- **CodeGraph First**: `RepoStoreBackend`, `RepoCore`, `RepoNode`, `RepoClient`, Python `RepoNodeApp`, and MiniNDN call paths were audited through CodeGraph before edits.
- **Spec-Driven Durable Work**: This feature owns requirements, contracts, tasks, validation, and convergence under Spec 073.
- **Verify With The Right Scope**: C++ and Python unit/smoke tests precede a real MiniNDN request/response test.
- **GSD**: Phase 5 records resumable implementation and acceptance state in `.planning/`.
- **ARS**: The experiment-agent workflow defines deterministic variables, controls, and pass/fail evidence in `experiment-plan.md`.
- **Second-pass review**: A minimal-context checklist was reviewed; architecture authority remains with the maintainers.

## Architecture

### C++ Storage Layer

`TieredRepoStore` implements `RepoStoreBackend` and owns:

1. an authoritative backing `RepoStoreBackend` (normally `SqliteRepoStore`);
2. an O(1) LRU map/list of complete `StoredObject` values;
3. a single mutex covering backing access, cache state, and counters;
4. one logical-byte budget shared by every cached `StoredObject`;
5. monotonic counters returned as `RepoCacheStatus`.

The store serializes backing and cache operations with one lock. This conservative choice avoids a stale read-through race in which an old SQLite value could be admitted after a concurrent overwrite. It can be split into finer locks only after measured contention justifies the added versioning complexity.

### Authority Ordering

```text
put/putManifest:
  authoritative write succeeds
  -> invalidate old cache entry
  -> admit new entry if eligible
  -> increment backingWrites/admissions

get:
  cache lookup
  -> hit: refresh LRU, return copy
  -> miss: authoritative read and integrity parsing
  -> admit if eligible
  -> return copy

erase:
  authoritative delete succeeds
  -> invalidate cache entry
  -> return authoritative result
```

Catalog changes remain in `RepoCore` and occur only after backend methods return successfully.

### Cache Charge and Eviction

The logical charge is deterministic:

```text
opaque object = payload bytes + object-name bytes + serialized-manifest bytes
packet set    = sum(wire bytes + Data-name bytes) + object-name bytes + serialized-manifest bytes
```

An entry with charge greater than the entire budget bypasses admission. Admission removes the previous value for the same typed key, inserts at MRU, then evicts LRU entries until `usedBytes <= budgetBytes`. A zero budget disables admission.

### Python Network Repo Parity

Replace the two independently evicted OrderedDicts with one `_BoundedRepoHotCache` owning typed keys (`object`, `packets`), one recency order, one budget, and the same counters. In persistent mode:

- do not duplicate all payloads in `LocalDistributedRepo`;
- commit SQLite transactions before cache/catalog publication;
- invalidate after successful SQLite delete/overwrite;
- use cache then SQLite for fetch;
- retain only temporary serving producers and retire them after a bounded serving interval.

One reentrant storage lock covers each cache lookup, SQLite transaction, and
subsequent cache transition. This prevents a concurrent reader from admitting
an older SQLite snapshot after an overwrite has installed a newer cache value.
Post-commit cache allocation failure invalidates any old entry and does not turn
a successful authoritative write into an application failure.

If no storage directory is supplied, the Python node derives a deterministic SQLite directory rather than selecting memory-only authority.

After restart, network reads activate packet serving on demand with
`FETCH_PREPARE` before invoking SegmentFetcher. Cache status and preparation
reuse the caller's existing `ServiceUser`; creating another runtime with the
same SVS identity would make later publications appear stale.

### Observability

`RepoCacheStatus`/`CACHE_STATUS` returns:

- `storageBackend`, `authoritativeBackend`, `cachePolicy`;
- `budgetBytes`, `usedBytes`, `entryCount`;
- `hits`, `misses`, `admissions`, `evictions`, `invalidations`;
- `oversizedBypasses`, `backingReads`, `backingWrites`.

Counters describe cache-layer operations, reset on process restart, and are snapshots rather than transactional audit records.

## Project Structure

### Documentation

```text
specs/073-repo-tiered-hot-cache/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── contracts/cache-status.md
├── checklists/requirements.md
└── tasks.md
```

### Source Code

```text
NDNSF-DistributedRepo/
├── include/ndnsf-distributed-repo/
│   ├── RepoTypes.hpp
│   ├── RepoProtocol.hpp
│   ├── RepoCore.hpp
│   ├── RepoNode.hpp
│   └── RepoClient.hpp
├── src/
│   ├── RepoTypes.cpp
│   ├── RepoProtocol.cpp
│   ├── RepoCore.cpp
│   ├── RepoNode.cpp
│   └── RepoClient.cpp
├── apps/DistributedRepoNodeApp.cpp
├── examples/
│   ├── DistributedRepoSmoke.cpp
│   └── DistributedRepoTieredCacheTest.cpp
├── configs/repo-node.conf
├── README.md
└── wscript

NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py
examples/python/NDNSF-DistributedRepo/generic_object_store/
├── repo_node.py
└── client.py
Experiments/NDNSF_DistributedRepo_Generic_Minindn.py
tests/python/test_ndnsf_repo_tiered_cache.py
```

**Structure Decision**: Extend the existing Repo backend interface and network adapters. Do not add a new daemon, database schema family, or application-specific cache API.

## Validation Sequence

1. Build the C++ DistributedRepo targets.
2. Run `DistributedRepoTieredCacheTest` and `DistributedRepoSmoke`.
3. Run focused Python cache/persistence tests.
4. Run existing Repo envelope and discovery tests.
5. Run the dedicated MiniNDN tiered-cache scenario.
6. Parse its JSON summary and logs for all required counters and integrity checks.
7. Re-run after fixes until every task and acceptance criterion passes.

The final MiniNDN run on 2026-07-10 passed all nine summary checks with
`hits=1`, `misses=4`, `backingReads=4`, `evictions=4`, and
`usedBytes=6200` under an 8192-byte budget.

## Complexity Tracking

No constitution violations require exceptions. The new wrapper backend is the smallest reusable place to enforce authority and cache ordering across all C++ Repo users.
