# Implementation Plan: High-Availability Concurrent Distributed Repo

**Branch**: `Experimental` | **Date**: 2026-07-10 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/077-repo-ha-concurrency/spec.md`

## Summary

Evolve NDNSF-REPO from a functional replicated research prototype into a measured high-concurrency, node-loss-tolerant repository. The implementation adds confirmed/idempotent writes, version/CAS metadata, a long-lived callback-backed exact Data producer, SQLite read/write concurrency and incremental capacity accounting, durable catalog/membership/repair state, health-aware placement and failover, bounded repair, runtime metrics, and a 60-second MiniNDN campaign. Existing exact signed Data names and wires remain canonical and Specs 073-076 remain compatibility gates.

## Technical Context

**Language/Version**: C++17; Python 3.8+

**Primary Dependencies**: ndn-cxx, NDNSF dynamic runtime, pybind11, SQLite3, Python `sqlite3`, MiniNDN, NFD/NLSR

**Storage**: Per-node SQLite WAL authority with bounded memory hot cache; durable local operation/catalog/repair metadata

**Testing**: C++ executables, Python `unittest`, NDNSF regressions, MiniNDN functional and 60-second performance campaigns

**Target Platform**: Ubuntu 20.04-compatible Linux Repo nodes and MiniNDN testbeds

**Project Type**: C++ shared library/runtime contracts plus Python network adapter, CLI/examples, and experiments

**Performance Goals**: Bounded producer/thread count; concurrent reads across unrelated objects; no full-table capacity scan per ACK; explicit stable-RPS envelope with p95/p99 evidence

**Constraints**: Preserve exact Data names/wires and NDNSF security; SQLite remains authoritative; do not use per-fetch producers or per-fetch NLSR advertisements on the scalable path; foreground traffic outranks repair; no proposal-slide edits

**Scale/Scope**: Initial validation with 3-5 Repo nodes, 1-32 clients, replication factor 1-3, read/write/mixed workloads, and one-node failure scenarios

## Constitution Check

- **Canonical Dynamic Runtime**: All control operations use the current generic NDNSF service API and unified Repo service name.
- **Security Is Part Of The Data Path**: Publisher checks, NAC-ABE, permissions, tokens, and replay protection remain enabled.
- **CodeGraph First**: RepoCore, RepoNode, RepoTypes, Python RepoNodeApp/client, producers, wrapper, and experiment paths were traced before design.
- **Spec-Driven Durable Work**: Requirements, contracts, tasks, validation, and convergence are owned by Spec 077.
- **Verify With The Right Scope**: Focused tests precede MiniNDN failure and 60-second performance campaigns.
- **GSD**: Phase 9 tracks this multi-stage implementation and acceptance state.
- **ARS**: The experiment-agent workflow defines variables, controls, warmup, duration, metrics, repetitions, and failure injection.

## Architecture

### 1. Confirmed Write Protocol

The client creates one `WriteIntent` before sending content. Every target Repo stores the intent and content transactionally, then returns a `WriteReceipt`. A receipt is valid only when operation ID, object name, generation, and digest match the intent. The client commits only after `requiredAcks` valid receipts.

```text
DISCOVER + optional capacity reservations
  -> create WriteIntent(operationId, generation, digest, RF, W)
  -> send idempotent write to selected replicas
  -> validate one durable WriteReceipt per replica
  -> receipts >= W: return committed VersionedManifest
  -> receipts < W: return incomplete write and enqueue repair/cleanup evidence
```

Legacy calls default to `W=RF`. Explicit `ONE` and `QUORUM` are available for experiments. Exact Data remains immutable. Mutable aliases use expected-generation CAS.

### 2. Always-On Repo Data Plane

Add a native `RepoDataPlaneProducer` owning one Face/event thread and a Python lookup callback. It registers many local Data prefixes on the same Face and registers one stable Repo forwarding route. On Interest, the callback performs exact-name lookup in hot cache/SQLite and returns the original wire bytes.

Opaque objects are segmented and signed once during first preparation; those serving packets and their prefix are persisted. Restart restores active serving prefixes. Exact packet sets use their original packet wires directly. No fetch creates a producer, thread, timer, or NLSR route.

### 3. Concurrent Storage Engine

- one serialized SQLite writer connection and bounded writer semaphore;
- thread-local read connections under WAL and `busy_timeout`;
- striped per-object locks for read/write cache coherence;
- incremental used/reserved-byte counters persisted in `repo_meta`;
- bounded foreground/repair admission and explicit fast rejection;
- latency histograms/EWMA for ACK telemetry.

The cache retains its own lock. A read captures the object generation under its stripe and only admits the result while the same stripe is held, preventing stale recache after overwrite.

### 4. Durable Catalog and Membership

Each process startup creates a boot incarnation. Catalog records are ordered by `(sourceRepo, sourceBootId, sourceSequence)` and stored in `catalog_journal`. Tombstones are durable. Peer watermarks and membership heartbeats are stored independently of object deltas.

Journal compaction retains the latest snapshot/tombstone per object generation and entries newer than the minimum peer acknowledgement. Bucket digests summarize deterministic object-name hash ranges for anti-entropy comparison.

### 5. Durable Repair Scheduler

Periodic scans derive under-replicated generations from live catalog entries. Repair jobs are inserted idempotently, leased for bounded intervals, retried with backoff, and completed only after a target write receipt is merged. Node loss alone triggers scans. Repair has separate concurrency and is denied admission before foreground work.

### 6. Placement, Reservations, and Reads

ACKs publish live used/reserved/free bytes, queue/inflight counts, cache state, storage latency, and existing network telemetry. Placement cache entries expire and are invalidated on failure. Failure-domain diversity remains mandatory when enough domains exist.

Clients keep per-replica EWMA health and cooldown. Each read has one total deadline divided into bounded attempts; Nack/timeout/integrity failures immediately penalize a replica. Packet-set failover always restarts the complete ordered set on one replica.

### 7. Command Status and Compatibility

Operation states are `RECEIVED`, `RUNNING`, `COMMITTED`, `INCOMPLETE`, `FAILED`, `CANCELLED`, and `EXPIRED`. Status records have retention/cleanup. These semantics align with standard Repo asynchronous insert/check behavior while remaining on the authenticated NDNSF control path. Full repo-ng command-wire parsing is intentionally isolated for a future adapter.

## SQLite Schema Additions

```text
repo_meta(key, value)
write_operations(operation_id PK, object_name, generation, expected_generation,
                 digest, replication_factor, required_acks, state, timestamps)
write_receipts(operation_id, repo_node, generation, digest, state,
               persisted_bytes, completed_at, PK(operation_id, repo_node))
serving_prefixes(prefix PK, object_name, generation, active)
serving_packets(data_name PK, object_name, generation, wire, wire_sha256, wire_size)
catalog_journal(source_repo, source_boot_id, source_sequence, object_name,
                generation, state, digest, entry_json,
                PK(source_repo, source_boot_id, source_sequence))
catalog_tombstones(object_name, generation, source_repo, source_boot_id,
                   source_sequence, entry_json, PK(object_name, generation))
peer_watermarks(peer_repo PK, peer_boot_id, source_sequence, updated_at)
repo_membership(repo_node PK, boot_id, last_sequence, last_seen, status_json)
repair_jobs(repair_id PK, object_name, generation, source_repo, target_repo,
            state, attempts, next_attempt, lease_owner, lease_deadline, result_json)
capacity_reservations(reservation_id PK, operation_id, bytes, state, expires_at)
```

## Project Structure

```text
specs/077-repo-ha-concurrency/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── contracts/
│   ├── write-commit.md
│   ├── data-plane.md
│   ├── catalog-repair.md
│   └── runtime-metrics.md
├── checklists/requirements.md
└── tasks.md

NDNSF-DistributedRepo/{include,src,examples,apps,configs}
pythonWrapper/{ndnsf/service.py,src/ndnsf/_ndnsf.cpp}
NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py
examples/python/NDNSF-DistributedRepo/generic_object_store/
Experiments/NDNSF_DistributedRepo_Generic_Minindn.py
tests/python/test_ndnsf_repo_ha.py
```

**Structure Decision**: C++ defines reusable contracts and native serving primitives. The existing Python adapter owns network orchestration and SQLite migration for current experiments. Both paths keep wire/field parity; duplicate high-level storage policy is not expanded further.

## Complexity Tracking

| Complexity | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| Native callback-backed producer | Serve arbitrary persisted exact Data with bounded Faces/threads | Static producer copies all packets into RAM and creates per-object threads |
| Durable local repair journal | Survive process/node loss without a cluster consensus service | In-memory sidecar sets lose work and suppress later repairs |
| Writer plus thread-local readers | Permit concurrent reads while preserving SQLite authority | One global connection/mutex serializes all traffic |
| Generation/CAS only for mutable metadata | Detect concurrent update conflicts | Last-write-by-clock can diverge and resurrect stale state |

## Delivery Phases

1. Contracts and SQLite migration.
2. Confirmed writes, receipts, CAS, and operation status.
3. Native always-on data plane and persisted serving packets.
4. Concurrent storage, incremental accounting, and runtime metrics.
5. Durable catalog, membership, compaction, anti-entropy, and repair jobs.
6. Dynamic placement, reservations, and bounded read failover.
7. Unit/integration regressions, MiniNDN correctness campaign, 60-second performance campaign, and convergence.
