# Feature Specification: Historical NDN-SVS Threading-Contract Recovery

**Feature Branch**: `134-svs-sync-crash-recovery`

**Created**: 2026-07-22

**Status**: Closed with one immutable `NOT_QUALIFIED` receipt. The
single-I/O-thread processes exited cleanly, but the MiniNDN runner failed to
install the remote publication route after an ignored duplicate-face
`Error 409`. Spec 134 was not rerun; the routing repair and verified RIB gate
were introduced under Spec 133's new evidence identity.

**Input**: Audit NDN-SVS itself before deciding how the old synchronous PubSub
experiment must run. Preserve the failed cross-thread diagnostics, stop the
unjustified library-lock repair, define the correct two-node/two-process
single-I/O-thread harness, and update Spec 133 without consuming or replacing
any formal cell.

## Source-Reality Boundary

The exact subject remains
`a9944019f76791773604999f00128057b9534ace`.

| Source | Verified fact | Experimental implication |
|---|---|---|
| `README.md` | Describes build/chat use but no threading contract | Cannot claim cross-thread publication from README |
| `ndn-svs/svspubsub.hpp` | Documents API behavior but no thread-safety guarantee or thread affinity | Public API does not authorize arbitrary-thread calls |
| `examples/chat-pubsub*.cpp` | Starts `Face::processEvents()` on a second thread, publishes from the application thread, and claims thread safety | This is an example claim, not a contract proven by API/tests |
| `tests/unit-tests/svspubsub.t.cpp` | Uses single-threaded `DummyClientFace`; no concurrency test | The example claim lacks regression coverage |
| Historical source | Uses partial mutex protection but no complete ownership policy for Scheduler, MappingProvider, MemoryDataStore, and notification state | High-rate cross-thread behavior is not safe evidence for the serial baseline |
| Spec 134 ASan/TSan results | Cross-thread harness triggers concrete Scheduler/container races | Proves an example/API/test mismatch; does not prove a fault in single-I/O-thread use |
| `a8a9656` and `15d1bc6` | Later parallel work explicitly posts worker results back to Face/io_context for serialized mutation/commit | Face/io_context ownership is the defensible baseline model |

The earlier Spec 134 repair commit and sanitizer outputs remain immutable local
diagnostic evidence. They MUST NOT be merged, promoted as the required old
implementation, or used as the subject for Spec 133.

## User Scenarios & Testing

### User Story 1 - Resolve the Threading Contract (Priority: P1)

As an evaluator, I can distinguish what README, API documentation, examples,
tests, implementation, and sanitizer evidence each prove, so the benchmark does
not turn an unsupported example claim into a production contract.

**Independent Test**: A source audit names the exact commit and anchors every
threading claim to a file/symbol. It explicitly records the contradictory
evidence and assigns the narrowest safe formal model.

**Acceptance Scenarios**:

1. **Given** the historical README/header, **when** thread safety is assessed,
   **then** silence is reported as unspecified rather than as support.
2. **Given** the chat example, **when** its thread-safe comment is assessed,
   **then** the lack of concurrent tests and sanitizer failure are reported.
3. **Given** the failed cross-thread runs, **when** causality is summarized,
   **then** they are labeled harness/contract-gap evidence and not formal
   throughput or single-I/O-path defect evidence.

### User Story 2 - Qualify the Intended Serial Execution Model (Priority: P1)

As an evaluator, I can run one fresh two-node MiniNDN qualification where every
peer keeps all NDN-SVS and application publication work on its own
Face/io_context thread, so Spec 133 starts from a defined and race-free
execution model.

**Independent Test**: Two MiniNDN nodes each launch one independent process.
Each process initializes one `Face` and one `SVSPubSub`, then runs one
Face/io_context thread. An absolute-deadline timer on that io_context invokes
synchronous `publish()` and re-arms itself. The same thread handles Sync,
Mapping, payload fetch, and subscription callbacks.

**Acceptance Scenarios**:

1. **Given** a peer process, **when** event processing begins, **then** no
   second thread calls Face, Scheduler, SVSync, or SVSPubSub.
2. **Given** publication work exceeds a period, **when** the timer callback
   returns, **then** missed release slots are counted and skipped; the harness
   does not enqueue a catch-up burst.
3. **Given** two peers, **when** qualification runs, **then** they reside on
   distinct MiniNDN nodes/processes and both publish and receive concurrently.
4. **Given** a crash, invalid payload, incomplete receipt, or corruption
   signature, **when** qualification ends, **then** the negative result is
   preserved once and is not retried automatically.

### User Story 3 - Hand the Correct Model Back to Spec 133 (Priority: P2)

As a researcher, I can resume the five-rate stage profiling only after the
single-I/O-thread model is qualified, without overwriting the failed
cross-thread preflight or changing the old NDN-SVS implementation.

**Independent Test**: Spec 133 documents the same process/thread/timer model,
marks its old driver/preflight tasks stale, keeps T009/T011 unchecked, and
requires a new manifest/result path for future execution.

## Requirements

### Functional Requirements

- **FR-001**: Spec 134 MUST use exact historical commit
  `a9944019f76791773604999f00128057b9534ace` plus only the canonical Boost 1.71
  build-compatibility patch for the qualification subject.
- **FR-002**: The active NDN-SVS checkout, Specs 131/132 results, Spec 133
  failed preflight, and existing Spec 134 sanitizer outputs MUST remain
  unchanged.
- **FR-003**: The audit MUST separately report README, public-header, example,
  unit-test, source, commit-history, and runtime-evidence facts.
- **FR-004**: Cross-thread sanitizer findings MUST be retained as measured
  evidence of a documentation/test/implementation mismatch and MUST NOT be
  generalized to the single-I/O-thread path.
- **FR-005**: The previously started synchronization repair MUST be abandoned
  as an experimental prerequisite. No old-version library source patch may be
  used by qualification or Spec 133.
- **FR-006**: Qualification MUST use exactly two MiniNDN nodes and one peer
  process per node; both processes publish and subscribe bidirectionally.
- **FR-007**: After initialization, each peer MUST execute application timer
  callbacks, synchronous `publish()`, Face processing, Sync/Mapping/payload
  work, and subscription callbacks on one Face/io_context thread.
- **FR-008**: The harness MUST contain no publisher/pacer thread, worker pool,
  `publishAsync()`, cross-thread `post` adapter, or second-thread NDN-SVS call.
- **FR-009**: The publication timer MUST use absolute monotonic deadlines,
  maintain at most one outstanding release, record actual callback lateness,
  skip/count elapsed release slots, and never issue a catch-up burst.
- **FR-010**: The qualification MUST use a deterministic 256-byte payload,
  target 1000 pps per peer, zero configured loss, 10-second warmup, 60-second
  measurement, and 10-second drain.
- **FR-011**: The receipt MUST report scheduled slots, attempted publications,
  missed release slots, attempted rate, remotely delivered publications,
  delivery/attempted ratio, invalid payloads, publication errors, process
  exits, and corruption-signature scan for both directions.
- **FR-012**: Local subscription delivery MUST be identified and excluded from
  remote-delivery and invalid-payload counters.
- **FR-013**: Every attempt MUST use a new path, exact subject/command/hash/
  linkage record, terminal receipt, no automatic retry, and bounded cleanup.
- **FR-014**: `QUALIFIED` requires two zero exits, complete receipts, nonzero
  bidirectional remote delivery, zero invalid remote payloads, zero publication
  errors, and zero corruption signatures. Reaching 1000 attempted pps is
  reported but is not a correctness requirement.
- **FR-015**: Spec 133 MUST be updated to use the same single-I/O-thread model,
  reset driver/runner/preflight tasks to incomplete at the recovery handoff,
  and preserve zero consumed formal cells until either this qualification is
  `QUALIFIED` or its terminal failure is proven to be infrastructure-only.
  An infrastructure-only release MUST keep the original receipt unchanged,
  repair the defect under a distinct Spec 133 build/result identity, verify the
  repaired prerequisite explicitly, and pass a fresh Spec 133 three-arm
  admission before any formal cell is consumed.
- **FR-016**: Spec 134 MUST make no throughput, latency, or bottleneck claim;
  its only terminal outcomes are `QUALIFIED`, `NOT_QUALIFIED`, or
  `INFRA_FAILURE`.

## Success Criteria

- **SC-001**: The source audit contains no unanchored thread-safety claim.
- **SC-002**: All Spec 133/134 documents agree on two nodes, two processes, and
  one Face/io_context execution thread per process.
- **SC-003**: Source-contract tests reject a second Face thread, direct
  main-thread `publish()`, `publishAsync()`, worker/pacer threads, and catch-up
  queues in the corrected qualification/profile drivers.
- **SC-004**: The single qualification produces one immutable terminal receipt
  satisfying every FR-011 field and FR-014 verdict rule.
- **SC-005**: No NDN-SVS library repair commit or patched runtime library is
  loaded by qualification or future Spec 133 execution.
- **SC-006**: Spec 133 remains blocked with zero formal cells until SC-004 is
  `QUALIFIED`, or until a `NOT_QUALIFIED`/`INFRA_FAILURE` receipt is preserved
  unchanged and its failure is isolated to infrastructure that is repaired and
  verified under a distinct Spec 133 evidence identity. In the latter case,
  Spec 133's fresh three-arm admission is the final formal-release gate.

## Assumptions

- Two MiniNDN host namespaces share the host monotonic clock.
- The qualification is a correctness/admission test, not a replicated
  statistical experiment.
- NFD remains a separate process per node; “single I/O thread” describes the
  application/NDN-SVS execution context, not the whole host.
