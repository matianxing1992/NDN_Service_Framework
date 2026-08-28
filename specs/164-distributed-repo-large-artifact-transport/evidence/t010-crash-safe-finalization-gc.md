# T010 Crash-Safe Finalization, Capacity, and GC Evidence

## Verdict

PASS. Final payload state and transactional metadata now cross their persistence
boundary through a durable replay journal. Startup deterministically completes
valid intents, leaves corrupt intents non-active for explicit rollback, and
reconciles interrupted garbage collection without deleting protected content.

## Finalization Protocol

SQLite schema generation 11 records one exact, bounded finalization row per
operation:

```text
INTENT_RECORDED
→ PAYLOAD_FINALIZED
→ METADATA_COMMITTED
→ ACTIVE
```

The intent contains the exact artifact/generation, logical name, policy epoch,
authenticated receipt envelope, and original commit time. The runtime executes:

```text
transactionally record intent and capacity owner
→ flush payload + range map
→ write filesystem finalize intent
→ atomic CAS rename + parent fsync
→ verify committed size and full digest
→ record PAYLOAD_FINALIZED
→ retain receipt + VERIFIED→COMMITTED in one transaction
→ catalog insert + COMMITTED→ACTIVE + journal completion in one transaction
```

Startup replays only recorded journal identities. It never activates based on a
filename alone. A CAS file found after rename but before sidecar cleanup is
reverified and its stale range/finalize sidecars are removed idempotently.

An unrecoverable pre-commit intent remains hidden with its error retained.
`rollback_finalization()` then records `FAILED`, releases capacity, clears
partial state, and removes finalized bytes only after metadata proves they are
unreferenced.

## Capacity and Temporary Ownership

- A reservation binds operation, artifact generation, lease, repository owner,
  charged bytes, actual bytes, and expiry.
- The admission check uses both the configured repository budget and current
  filesystem free space.
- Charges include bounded configured overhead. Already verified CAS bytes are
  deduplicated and charge only overhead.
- Renewal requires a fresh lease and later expiry. Failure, destructive
  cancellation, rollback, and GC release the reservation.
- Commit accounts unique active content identities rather than logical-name
  aliases.

## Garbage Collection Protocol

GC claims one exact digest/generation with an exclusive owner and deadline.
Every claim and reclaim transaction rechecks:

- active catalog references;
- retained committed receipts;
- nonterminal finalization journals;
- active/finalizing/committed capacity ownership;
- unexpired transfer leases.

Only expired, failed, or expired-preserving-cancelled temporary sessions are
eligible. Reclaim first records `RECLAIMING`, removes only staging state, then
records the lifecycle terminal state, releases capacity, and records
`RECLAIMED`. Startup idempotently finishes a crash after payload removal but
before metadata completion.

## Failure-Injection Evidence

`tests/python/test_spec164_crash_recovery.py`:

```text
7/7 PASS
```

Covered boundaries:

1. crash after durable finalization intent;
2. crash after atomic payload rename but before SQLite payload-phase update;
3. crash after `PAYLOAD_FINALIZED`;
4. crash after receipt retention and metadata commit but before activation;
5. crash after activation;
6. stale filesystem sidecars left immediately after rename;
7. corrupt verified staging bytes during startup replay;
8. GC crash after staging deletion but before ownership/lifecycle completion.

Every valid finalization reaches the exact five-state lifecycle
`RESERVED, RECEIVING, VERIFIED, COMMITTED, ACTIVE` after reopen. Before reopen,
only the post-activation injection is discoverable. Corrupt recovery remains
non-active and rolls back to `FAILED`.

Additional cases prove configured low-space rejection before lifecycle
reservation, exact committed-byte accounting, active/receipted GC protection,
unexpired-lease protection, exclusive GC ownership, and automatic collection
of an abandoned OPEN session after lease expiry.

## Regression Evidence

```text
All Spec 164 Python tests                         52/52 PASS
Exact-packet compatibility                       12/12 PASS
Native manifest/transfer/filesystem-store tests  20/20 PASS
git diff --check                                 PASS
Spec Kit strict structural audit                 PASS
```

## Security and Claim Boundary

The authenticated receipt is generated and verified before its replay material
enters the trusted local metadata authority. Recovery requires byte-identical
intent/receipt identity and a fully reverified CAS payload. Protection against
an attacker with arbitrary write access to both the repository database and
filesystem is outside this local-storage trust boundary.

This is deterministic local crash-injection evidence, not process/network
interruption or multi-replica evidence. T011 owns those MiniNDN cases,
including injected low-space behavior on the network path and achieved
durability from distinct retained receipts.

## Five-Tool Gate

- Context Mode: the health guard failed closed because the project ContentDB
  remains absent; repository documents and tests were authoritative.
- CodeGraph: synchronized and inspected finalization, recovery, capacity, GC,
  and lifecycle call paths after implementation.
- Spec Kit: strict structural audit passed.
- GSD: health remained valid with only the unrelated phase-34 informational
  note.
- ARS: not applicable to this implementation-only task; no experiment claim
  was made.
