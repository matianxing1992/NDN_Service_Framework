# Research Decisions: SQLite-Authoritative Repo Hot Cache

## Decision 1: SQLite Is the Only Persistent Authority

**Decision**: Tiered mode reports success only after the SQLite backend method completes. Cache and catalog updates happen afterward.

**Rationale**: It prevents acknowledged cache-only data and makes restart behavior deterministic.

**Alternatives considered**:

- Write-back cache: rejected because crash recovery, dirty tracking, and flush ordering expand scope and weaken durability.
- Memory authority with periodic snapshots: rejected because it does not meet the user's explicit SQLite-authoritative requirement.

## Decision 2: One Shared Byte-Bounded LRU

**Decision**: Use one typed LRU namespace and one logical-byte counter for opaque objects and segmented packet sets.

**Rationale**: The current Python implementation has two eviction loops sharing a byte counter; either cache can prevent the other from evicting enough entries. A global LRU makes the configured limit enforceable and predictable.

**Alternatives considered**:

- Separate quotas: rejected for the first version because operators would need two budgets and unused capacity could not be shared.
- LFU/ARC/TinyLFU: deferred until traces show LRU admission pollution is a real problem.

## Decision 3: Serialize Backing and LRU Operations

**Decision**: Use one store-level mutex around each authoritative/cache operation in C++ and an equivalent re-entrant lock in Python.

**Rationale**: This prevents an old miss result from being admitted after a newer overwrite and establishes one lock order with no deadlock cycle.

**Alternatives considered**:

- Independent SQLite/cache locks: rejected because correct concurrent read-through would also need generation/version checks.
- Lock-free counters and cache: rejected because complexity is not justified by current Repo throughput evidence.

## Decision 4: Complete-Object Admission

**Decision**: Cache complete opaque objects or complete packet sets as atomic entries. Manifest-only C++ records may be cached as complete zero-payload records.

**Rationale**: Whole-entry eviction cannot expose a partially cached packet set as complete.

**Alternatives considered**:

- Per-segment cache: deferred; it requires missing-range state and a second assembly policy.

## Decision 5: Persistent Python Repo Must Stop Duplicating All Data

**Decision**: When SQLite is configured, `LocalDistributedRepo` is not a second unbounded payload store. The bounded hot cache is the only process-local retained copy outside temporary active producers.

**Rationale**: Keeping every payload in `_store` means reads bypass SQLite and the configured cache limit is not meaningful.

**Alternatives considered**:

- Leave `_store` populated only for compatibility: rejected because it invalidates the core memory-bound requirement.

## Decision 6: Expose a Dedicated CACHE_STATUS Operation

**Decision**: Add a generic Repo status contract rather than overloading catalog status or capability fields.

**Rationale**: Catalog state is object-discovery control plane; cache counters are local storage-runtime telemetry. A separate operation keeps both contracts coherent.

**Alternatives considered**:

- Add counters only to startup logs: rejected because MiniNDN clients cannot verify behavior.
- Add all counters to capability ACKs: rejected because frequent counter changes would bloat normal selection metadata.

## Design Review Findings

The design review identified stale read-through races, cache admission failure after a successful commit, partial segmented-write concerns, zero-budget behavior, and status consistency. The design adopts serialized operation ordering, treats cache admission as optional acceleration after durability, and tests zero/oversized/failure cases. Existing multi-record segmented insertion atomicity remains outside this cache feature; individual committed records remain authoritative and the parent manifest is published only after completion.
