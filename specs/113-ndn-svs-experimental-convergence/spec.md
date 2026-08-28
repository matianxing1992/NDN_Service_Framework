# Feature Specification: NDN-SVS Experimental Convergence

**Feature Branch**: `Experimental`

**Created**: 2026-07-15

**Status**: Complete

**Input**: Preserve the current NDN-SVS Experimental work, align local master
with the unchanged remote master, rebuild Experimental as a clean branch based
on that master, organize its real additions into reviewable commits, correct
the merge-readiness defects found by audit, and iterate until every declared
task and validation gate is complete.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Preserve Every Existing Change (Priority: P1)

The maintainer can reorganize the branch without losing the old Experimental
history, its tracked worktree changes, or the local design artifacts that
explain those changes.

**Why this priority**: Branch reconstruction is destructive unless the original
state has an immutable recovery point.

**Independent Test**: Compare a recorded pre-migration manifest with the backup
branch and verify that every original commit, tracked diff, and selected local
design artifact remains recoverable after reconstruction.

**Acceptance Scenarios**:

1. **Given** the current dirty Experimental worktree, **When** migration begins,
   **Then** an immutable backup records the original HEAD, tracked diff, relevant
   ignored design files, and repository status before any branch reference moves.
2. **Given** a conflict or failed validation during reconstruction, **When** the
   maintainer returns to the backup, **Then** the complete pre-migration state can
   be recovered without relying on a transient stash.

---

### User Story 2 - Establish a Reviewable Branch Topology (Priority: P1)

The maintainer sees local master aligned exactly with the current remote master,
while Experimental contains only the cleaned, intentional changes that remain
after the remote reviewed history.

**Why this priority**: The existing branches contain rewritten equivalent
history and cannot be merged or reviewed reliably.

**Independent Test**: Inspect the final commit graph and prove that local master
equals the recorded remote-master commit, Experimental descends from that same
commit, the remote master was not changed, and the backup still names the old
state.

**Acceptance Scenarios**:

1. **Given** the reviewed remote master and divergent local branches, **When**
   reconstruction completes, **Then** local master and the recorded remote
   master resolve to the same commit.
2. **Given** Experimental contains old equivalents of reviewed commits, **When**
   its history is rebuilt, **Then** those equivalents are not replayed and only
   genuine deltas remain above the remote-master baseline.
3. **Given** no remote publication was authorized, **When** all local work
   completes, **Then** the remote master and remote Experimental references are
   unchanged.

---

### User Story 3 - Review and Revert Changes by Concern (Priority: P1)

Reviewers can understand, test, and revert each surviving NDN-SVS change without
unpacking one mixed Spec 110/112 or workflow-configuration commit.

**Why this priority**: A functionally passing but inseparable history is not a
safe merge candidate.

**Independent Test**: Review the final commit sequence and confirm that each
commit has one stated purpose, carries its own relevant tests, and excludes
local-only workflow ignores and unrelated mechanisms.

**Acceptance Scenarios**:

1. **Given** mapping, recovery, segmentation, lifecycle, and build-baseline
   changes, **When** the final history is inspected, **Then** each concern has an
   independently reviewable and revertible commit boundary.
2. **Given** a commit described as experimental, **When** it is retained for
   master review, **Then** its behavior is either promoted with a precise stable
   contract and tests or excluded from the merge candidate.

---

### User Story 4 - Preserve Publication Correctness After Reconstruction (Priority: P1)

Applications receive segmented publications without oversized packets,
sequence gaps, poisoned providers, unsafe callback lifetime, or a hidden change
to the documented asynchronous execution contract.

**Why this priority**: Reorganizing history is unacceptable if it preserves or
introduces a distributed-correctness defect.

**Independent Test**: Inject preparation, storage, emission, commit, validation,
and shutdown failures; exercise exact packet-size boundaries and multiple queued
publications; then verify rollback, sequence continuity, provider health, and
documented thread ownership.

**Acceptance Scenarios**:

1. **Given** one asynchronous publication fails during its commit stage,
   **When** later publications are ready, **Then** no commit cursor advances past
   unreadable state and every failed or uncommitted packet is reclaimed.
2. **Given** a caller uses asynchronous publication, **When** work crosses thread
   boundaries, **Then** the observed behavior agrees with the public contract and
   all network-face operations execute on their owning event-loop thread.
3. **Given** a custom publication store, **When** asynchronous publication can
   leave stored but not-yet-advertised Data, **Then** the store either provides
   enforceable exact-name rollback or the publication is rejected before its
   first insert, including the single-packet case.
4. **Given** final encoded packets at, below, and above the active limit, **When**
   segmentation is tested, **Then** the actual signed outer packets prove the
   boundary and no oversized packet is emitted.

---

### User Story 5 - Produce Fresh Merge Evidence (Priority: P2)

The maintainer can demonstrate that the reconstructed sources—not an earlier
dirty candidate—build and pass the required local and MiniNDN acceptance paths.

**Why this priority**: Existing evidence is source-identity bound and cannot be
silently transferred to rewritten commits.

**Independent Test**: Bind a new candidate to the final branch, binaries,
configuration, and test scripts; run every declared cell once; and verify that
the retained summaries meet the acceptance counts.

**Acceptance Scenarios**:

1. **Given** the source identity changes during history reconstruction, **When**
   final validation begins, **Then** a new immutable candidate is created rather
   than relabeling prior evidence.
2. **Given** all required unit and network cells finish, **When** evidence is
   audited, **Then** commands, hashes, outcomes, negative results, and branch
   topology are sufficient to reproduce the merge decision.

### Edge Cases

- The remote master changes externally after the baseline is recorded.
- A reviewed remote commit has the same intent but a non-equivalent patch.
- Conflict resolution accidentally restores the superseded custom Interest
  signer instead of the reviewed remote implementation.
- The backup includes local workflow documents that must not enter the product
  history.
- A commit-stage exception occurs after several later publications are prepared.
- A custom DataStore cannot erase a partially inserted publication.
- The packet limit changes from the currently observed value.
- Reordered commits produce different source and binary identities even when
  behavior appears unchanged.
- MiniNDN startup fails before a valid test cell begins; infrastructure failure
  must not be relabeled as a product result.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The migration MUST create an immutable recovery point containing
  the original Experimental commit graph, tracked worktree changes, status, and
  relevant local design artifacts before modifying branch references.
- **FR-002**: The migration MUST NOT rely on a transient stash as its only
  recovery mechanism.
- **FR-003**: Final local master MUST resolve to the recorded current
  remote-master commit.
- **FR-004**: Final Experimental MUST descend from that remote-master commit and
  MUST contain only intentional deltas not already represented by reviewed
  remote history.
- **FR-005**: The operation MUST NOT push, force-push, merge into, or otherwise
  mutate the remote master.
- **FR-006**: The old Experimental state MUST remain named by a permanent backup
  reference after the new Experimental is established.
- **FR-007**: Mapping behavior, bounded recovery, segmented publication
  correctness, callback lifetime, and Boost-baseline changes MUST have distinct,
  reviewable ownership and commit boundaries.
- **FR-008**: Local-only agent, planning, result, and ignored-document rules MUST
  NOT be mixed into the product merge candidate.
- **FR-009**: Reviewed remote implementations MUST be retained unless an explicit
  test-backed requirement justifies replacing them.
- **FR-010**: An asynchronous commit failure MUST NOT advance the visible
  sequence or commit cursor past the failed publication and MUST reclaim failed
  and blocked prepared state deterministically.
- **FR-011**: Public asynchronous publication documentation, observable return
  behavior, worker execution, and network-face thread ownership MUST agree.
- **FR-012**: Atomic publication rollback MUST be enforceable for every accepted
  DataStore implementation; unsupported rollback MUST fail explicitly rather
  than silently succeeding.
- **FR-013**: Boundary tests MUST construct and verify actual final signed outer
  packets at one byte below, exactly at, and one byte above the active packet
  limit.
- **FR-014**: The reconstructed sources MUST configure and build on the declared
  local Boost 1.71 baseline without weakening the separate upstream publication
  policy.
- **FR-015**: The complete NDN-SVS unit suite and focused failure tests MUST pass
  from rebuilt binaries produced from the final source identity.
- **FR-016**: Cross-repository NDNSF tests MUST verify that the final NDN-SVS
  library preserves normal and Targeted segmented responses, timeout behavior,
  tokens, and provider health.
- **FR-017**: Final MiniNDN evidence MUST bind the exact final sources, dirty
  state, binaries, scripts, configuration, branch graph, and candidate identity.
- **FR-018**: Each final candidate/cell pair MUST execute once; invalid startup
  attempts and negative results MUST remain retained and accurately classified.
- **FR-019**: Completion MUST include a post-implementation code-aware audit and
  convergence pass with no remaining Critical or High finding.
- **FR-020**: Final Experimental MUST have a clean tracked worktree, an explicit
  upstream or documented no-upstream disposition, and no unresolved merge
  conflict markers.

### Key Entities

- **Migration Baseline**: Recorded remote-master commit, original Experimental
  commit, local-master commit, timestamps, status, and diff identities.
- **Recovery Snapshot**: Permanent backup reference plus the commit that captures
  the pre-migration tracked and selected local-only state.
- **Review Commit**: One purpose, its prerequisite, affected ownership area,
  associated tests, and rollback boundary.
- **Publication Transaction**: Reserved sequence, prepared packets, store state,
  event-loop commit state, visible sequence, and terminal rollback/commit result.
- **Validation Candidate**: Immutable source, binary, configuration, script,
  branch-topology, cell, and outcome identity.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The backup manifest accounts for 100% of pre-migration commits,
  tracked modifications, and selected local design artifacts.
- **SC-002**: Local master and the recorded remote master have identical commit
  IDs, while Experimental has that commit as an ancestor and the remote refs
  remain unchanged by this work.
- **SC-003**: Zero reviewed-equivalent historical commits are duplicated above
  the new Experimental baseline.
- **SC-004**: Every final commit has exactly one review concern, relevant passing
  tests, and an independently describable rollback effect.
- **SC-005**: Injected commit-stage failure produces zero visible sequence gaps,
  zero later commits past the failure, zero leaked prepared packets, and a
  successful subsequent recovery publication.
- **SC-006**: Actual signed outer packets prove the below/exact/above boundary;
  all emitted packets are within the active limit.
- **SC-007**: A clean Boost 1.71 configuration and build completes, and 100% of
  the NDN-SVS unit suite passes.
- **SC-008**: The fresh reconstructed-source MiniNDN candidate passes all
  declared Spec 112 boundary, burst-health, and degraded-target timeout cells
  with their original acceptance counts.
- **SC-009**: Final audits report zero Critical and zero High finding, all tasks
  are complete, and convergence appends no further work.

## Assumptions

- `origin/master` at the recorded `db9fc25` baseline is the local representation
  of the remote master to preserve; an external remote change triggers a new
  baseline decision instead of an automatic rebase.
- Existing Spec 112 evidence remains historical evidence but is not accepted as
  final proof for rewritten source identities.
- The reviewed remote InterestSigner implementation is preferred over the old
  local custom timestamp implementation.
- No remote push is authorized by this specification.
- MiniNDN remains the final network-validation environment; Docker, iTiger,
  real Wi-Fi, throughput optimization, and unrelated NDNSF-DI work are outside
  this feature.

## Explicitly Out of Scope

- Publishing or merging the final branch to GitHub.
- Rewriting the remote master or remote Experimental history.
- New NDN-SVS wire mechanisms beyond those already present in the retained
  Spec 110 delta.
- General performance tuning or 5% loss campaigns.
- Changes to NDNSF-DI, UAV, Repo, Docker, or iTiger behavior.
