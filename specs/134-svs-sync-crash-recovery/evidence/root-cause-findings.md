# Spec 134 Root-Cause Findings

> **Reclassified after the NDN-SVS source/README audit**: these findings are
> confirmed for the historical example-style cross-thread harness. They prove
> a documentation/test/implementation mismatch, not a defect on the corrected
> single-Face/io_context-thread experiment path. The repair direction is
> withdrawn as a prerequisite; all artifacts remain immutable.

## Diagnostic attempts

| Attempt | Terminal result | Peer exits | Primary evidence |
|---|---|---:|---|
| `diagnosis-asan-01` | `SUBJECT_FAILURE` | 134, 134 | ASan SEGV/double-free in ndn-cxx Scheduler event queue |
| `diagnosis-tsan-01` | `SUBJECT_FAILURE` | 66, 66 | TSan race on `m_notificationMappingList.pairs` |

Both were fresh Spec 134 MiniNDN attempts at target 1000 publications/s per
peer. Neither was retried. Receipts and raw logs are under
`results/spec134-svs-sync-crash-recovery/`.

## RC-001 — CONFIRMED: Scheduler event queue accessed across threads

- **Shared object**: the ndn-cxx `Scheduler` event multiset owned by
  `SVSyncCore::m_scheduler`, including `m_retxEvent` cancellation/replacement.
- **Application-main operation**: synchronous `SVSPubSub::publish` calls
  `SVSyncBase::publishData`, then `SVSyncCore::updateSeqNo`, then
  `retxSyncInterest(false, 1)`. At `core.cpp:237` it assigns a newly scheduled
  event, which cancels/replaces the prior event.
- **Face-I/O operation**: the scheduler executes/cancels the same event queue on
  the thread running `Face::processEvents`.
- **Violated invariant**: ndn-cxx `Scheduler` queue mutation is confined to its
  owning I/O-context thread. `m_schedulerMutex` protects only NDN-SVS call
  sites; it cannot protect Scheduler's internal Face-thread event execution.
- **Evidence**: peer B ASan reports a double-free of the same red-black-tree
  event node, allocated by T0, freed by T2, and freed again by T0. Peer A
  reports a null write in `_Rb_tree_insert_and_rebalance` on the same
  `Scheduler::schedule -> retxSyncInterest -> updateSeqNo -> publish` path.

## RC-002 — CONFIRMED: notification Mapping list read/write race

- **Shared object**: `SVSPubSub::m_notificationMappingList.pairs`.
- **Application-main operation**: `SVSPubSub::insertMapping` appends at
  `svspubsub.cpp:161` after synchronous publication.
- **Face-I/O operation**: `SVSPubSub::onGetExtraData` iterates the same vector at
  `svspubsub.cpp:642` while producing a Sync Interest.
- **Violated invariant**: the existing `m_extraDataMutex` is held by the reader
  but not by the writer, so vector size/storage can be read while reallocated.
- **Evidence**: TSan identifies the Face-thread read and main-thread write,
  including thread creation at the driver's `Face::processEvents` thread.

## Unconfirmed candidates

- `MappingProvider::m_map` has no class-local lock and has main/Face call paths.
- `MemoryDataStore::m_ims` has no class-local lock and has main/Face call paths.
- direct `Face::put` from synchronous publication may require thread affinity.

These remain `CANDIDATE`; the baseline TSan attempt intentionally halted at its
first race. They are not investigated by the corrected formal model because it
does not make concurrent NDN-SVS calls.

## Withdrawn repair direction

The earlier proposed locks/posting and the local experimental repair remain
forensic artifacts only. They are not authorized for merge or qualification.
The corrected path keeps application publication and all NDN-SVS work on one
Face/io_context execution thread and qualifies the unmodified historical
runtime instead.
