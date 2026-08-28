# Contract: Transactional Asynchronous Publication

## Public Semantics

`publishAsync` preserves its existing signature and returns a reserved sequence
only after safe local preparation completes. It is asynchronous with respect to
event-loop advertisement, not with respect to signing, final encoding, or
complete local storage.

## Thread Ownership

- Callers may prepare from their current thread under the publication ordering
  lock.
- `Face::put`, state-vector advertisement, and event-loop retry scheduling occur
  only on the owning Face `io_context`.
- Public documentation and tests state this division explicitly.

## Commit Ordering

- The next-commit cursor names the first not-yet-advertised sequence.
- Removing a ready transaction and advancing the cursor occur only after its
  mapping and local version-vector transition succeed.
- A failed head remains at the head in `retry-wait`; later transactions remain
  prepared and blocked.
- Retry before the local commit point is bounded in frequency and does not
  busy-loop.
- A network Sync send exception after the local version-vector transition is
  contained and left for subsequent/periodic Sync; it cannot unwind the local
  commit or leave the cursor behind visible state.
- Shutdown aborts and removes all stored but unadvertised transactions.

## Storage Atomicity

- All final packet encodings are checked before the first insert.
- Partial insertion is erased in reverse order.
- Every accepted transactional DataStore advertises rollback capability and
  implements exact-name erase.
- Existing stores default to unsupported for source compatibility; every
  asynchronous transaction that can leave stored but unadvertised Data rejects
  them before its first insert, including a single-packet transaction.
- Rollback failure is a hard invariant failure and is observable; it is never
  silently reported as successful atomic rollback.

## Packet Boundary

- Segment fitting uses actual signed inner and signed outer wire sizes.
- Tests construct actual final outer packets at limit-1, limit, and limit+1.
- The first two are accepted; the last is rejected or split before emission.

## Optional Active Emission

An active first-packet `Face::put` is an optimization. Failure is logged and the
readable stored publication remains available through normal fetch after state
advertisement; it does not corrupt the transaction or sequence ordering.
