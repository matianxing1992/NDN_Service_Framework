# T009 Exact-Identity Resumable Sessions Evidence

## Verdict

PASS. Publication and retrieval now retain bounded durable checkpoints, resume
only the exact artifact/root/packet/chunk identity, transfer only missing work,
and fail closed on stale leases, mixed identity, progress regression, or
attempted overwrite of verified bytes.

## Implemented Closure

1. Native `ArtifactResumeSession` validates the full `ArtifactReference`,
   manifest-root digest, packet/chunk geometry, exact contiguous chunk cover,
   segment coordinates, operation/repository lease binding, and bounded chunk
   count before work starts.
2. Native progress is monotonic. Restored chunks are excluded from
   `missingChunks`; duplicate verified chunks increase
   `avoidedRetransmissionBytes` without increasing verified progress.
3. Every work-producing call checks lease expiry. Renewal and resume require a
   later expiry plus fresh lease and replay identities for the same repository,
   operation, and artifact.
4. Preserving cancellation retains verified ranges. Destructive cancellation
   clears staging bytes and makes the session non-resumable.
5. SQLite schema generation 10 adds one bounded checkpoint row per operation,
   not one row per packet. Its transaction enforces immutable resume identity,
   allowed state transitions, monotonic progress/metrics, and lease-renewal
   rules.
6. `ArtifactReplicaSession` reconstructs verified chunk indices from the
   streaming CAS range sidecar after restart. A range made durable before a
   checkpoint crash is recovered; a checkpoint claiming absent durable ranges
   is rejected.
7. Consumer destinations use a deterministic partial file and atomically
   replaced, fsynced resume sidecar. Reopen derives only missing ranges.
   Byte-identical duplicate writes are idempotent; partial overlap or changed
   verified bytes fail closed.
8. Final consumer visibility remains a no-overwrite atomic hard-link after
   complete coverage and full SHA-256 verification. Partial files remain
   hidden.

## Recovery Ordering

Publication:

```text
verify chunk digest
→ bounded pwrite
→ atomically persist verified range
→ atomically persist monotonic session checkpoint
```

If interruption occurs between the last two steps, restart reconstructs the
newer verified set from the CAS range sidecar and advances, never regresses,
the session checkpoint.

Retrieval:

```text
bounded pwrite
→ fsync partial payload
→ atomically replace + fsync resume sidecar
→ on complete coverage, verify full digest
→ no-overwrite destination commit
```

## Deterministic Validation

```text
Native ArtifactManifest + ArtifactTransfer + FilesystemArtifactStore
  20/20 PASS

Spec 164 Python discovery
  44/44 PASS

Exact-packet compatibility
  12/12 PASS

T009 focused Python resume suite
  5/5 PASS

Core Python extension resource build
  PASS (-O0 -g0, -j1)

git diff --check
  PASS

Spec Kit strict structural audit
  PASS (9/20 tasks complete)
```

Focused cases prove:

- interruption after chunk 0 of 2, restart, and a missing set containing only
  chunk 1;
- exact transferred-byte accounting: 8 newly verified bytes and 4 bytes of
  avoided duplicate traffic;
- in-place renewal, expiry, fresh-lease resume, preserving cancellation, and
  destructive cancellation;
- durable root-identity substitution rejection;
- retrieval restart with only the unverified range requested;
- byte-identical retrieval retry, conflicting verified-range rejection,
  changed-identity rejection, and cleanup semantics.

## Claim Boundary

T009 is deterministic local recovery implementation evidence. It makes no
network-performance or crash-matrix claim. T010 owns crash-point reconciliation,
capacity, temporary ownership, and GC. T011 owns process interruption and
multi-replica MiniNDN evidence.

## Five-Tool Gate

- Context Mode: anomaly statistics were collected earlier; the repository
  health guard again failed closed because no project ContentDB exists.
  Repository artifacts remained authoritative.
- CodeGraph: synchronized and verified the native-to-Python resume flow,
  persistence call path, and test blast radius.
- Spec Kit: strict structural audit passed.
- GSD: installation health passed with no errors or warnings; the unrelated
  phase-34 missing summary remains informational.
- ARS: not applicable to this implementation-only task; no experiment or
  research claim was created.
