# Contract: Payload and Metadata Backends

## Authoritative Boundary

The deployed repository has one authoritative backend facade. Python
orchestration, native bindings, and C++ runtime cannot independently commit
different lifecycle state for the same artifact.

## PayloadStore

Required operations:

- begin or resume temporary storage for an exact artifact generation;
- write a bounded range idempotently;
- read a bounded range;
- mark a chunk/range verified only after digest validation;
- query verified progress;
- fsync/durably flush according to policy;
- atomically finalize to immutable content-addressed storage;
- test committed existence and exact size/identity;
- abort or reclaim an owned temporary generation.

The scalable backend stores payload bytes as files or equivalently contiguous
objects, not one durable metadata BLOB per small NDN Data packet.

## MetadataStore

Required transactional operations:

- capabilities and schema generation;
- artifact catalog and logical-name versions;
- root/page manifest references;
- queued-task, internal transfer-session, and verified-progress state;
- finalization journal;
- replica receipts and achieved durability;
- policy/revocation context;
- garbage-collection ownership and deadlines.

Metadata work scales with artifacts, sessions, replicas, chunks/pages, and
lifecycle transitions rather than 4 KiB packet count.

## Atomicity Protocol

Payload finalization and metadata activation span two persistence domains.
The backend uses a durable idempotent finalization intent:

1. transactionally record intent and exact artifact identity;
2. flush and atomically rename/finalize payload;
3. transactionally record committed payload generation and receipt;
4. activate catalog after durability policy;
5. clear or retain completed journal according to audit policy.

Recovery replays these steps based on the journal and verified identity, never
on filename presence alone.

## Initial Backend Choice

The initial scalable subject is:

- content-addressed payload files;
- temporary `.part` generations outside the active namespace;
- embedded transactional metadata with WAL/batched transactions;
- indexed point/range queries and no global scan on each Data packet.

This choice is a benchmark subject, not a permanent engine mandate.

## Alternative Metadata Engines

An alternative is introduced only if the corrected workload demonstrates a
metadata bottleneck. Evaluation must include throughput, p95 commit latency,
CPU, memory, read/write/space amplification, crash recovery, operational
complexity, and migration—not write throughput alone.

## Legacy Backend

`exact-packet-v1` retains exact wire-packet storage and retrieval semantics
behind an explicit backend/format. New scalable objects do not mutate or
reinterpret those rows.

## Deduplication

Committed payload bytes may be shared by full content digest. Catalog,
publisher provenance, logical-name authorization, policy epoch, and receipts
remain separate records. Reference counting or reachability protects bytes
from garbage collection while any valid catalog/session generation owns them.

## Capacity and Garbage Collection

- Advisory ACK capacity is never persisted as a reservation. Execution checks
  current storage limits when a queued task starts and fails explicitly if
  capacity is no longer available.
- Actual bytes and overhead are measured.
- Temporary generations have lease owner and expiry.
- GC acquires exclusive generation ownership.
- Active, committed, referenced, or leased payloads are never reclaimed.
- Low-space behavior rejects or pauses safely before corrupting committed data.

## Migration and Rollback

- Schema generations are explicit.
- Upgrade never rewrites legacy payload automatically.
- New format writes are capability-gated.
- Rollback may disable new writes but preserves opaque new-format records.
- Destructive conversion or deletion requires a separate operator-authorized
  migration outside automatic startup.
