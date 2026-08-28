# Feature Specification: Complete SVS V3 Review Commit

**Feature Branch**: `115-ndn-svs-v3-review-history`

**Created**: 2026-07-16

**Status**: Nine-owner local history correction complete; exact-head
publication/CI remains on explicit user hold

**Input**: Rewrite the open NDN-SVS review history so that one complete,
standards-compliant V3 commit contains every official V3 upgrade and correction,
and that commit is visible from `Experimental`, local `master`, and
`origin/master` after completion.

## 2026-08-07 Focused PR Override

This section supersedes only the earlier commit-order, PR-boundary, and exact
final-tree requirements where they conflict with the user's current direction.
Historical evidence and safety refs remain evidence; they are not the source
branch for the new pull request.

The complete local history remains behavior-owned, but its first three commits
MUST be, in order:

1. regex subscriptions and name-only PubSub publication;
2. V2 signed Sync Interest encoding through ndn-cxx `InterestSigner`;
3. complete SVS V3 synchronization, including the current official SVS-PS V3
   naming and signed MappingData placement corrections.

The third commit is the local pull-request boundary. A dedicated local branch
MUST point exactly to it. Mapping optimization, parallel Sync, asynchronous
publication, sparse Mapping recovery, failure-atomic segmented publication,
and bounded Fetcher/Repair recovery remain descendant commits on a separate
continuation branch and MUST NOT appear in the focused V3 pull-request diff.

NDN-SVS MUST contain only reviewable library source, normal project examples,
unit tests, and deterministic protocol fixtures. Planning, audit, experiment,
interop orchestration, generated evidence, and project-specific documentation
remain under `/home/tianxing/NDN/ndn-service-framework`.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Review One Complete V3 Change (Priority: P1)

As an upstream reviewer, I can inspect one cohesive V3 commit that contains the
complete official V3 protocol behavior and its directly coupled tests, rather
than reconstructing the final behavior from an incomplete historical commit
and several later fixes.

**Why this priority**: PR #36 is blocked because its current V3 commit produces
a hybrid packet that is compatible with neither the official V2 nor V3 wire
format.

**Independent Test**: Check out the replacement V3 commit by itself, build it,
run its V3 unit/fixed-vector gates, and inspect its diff. The checkout produces
and accepts the normative V3 envelope without relying on a descendant fix.

**Acceptance Scenarios**:

1. **Given** the current incomplete V3 commit, **When** the review history is
   rewritten, **Then** exactly one replacement commit owns all official V3
   production behavior and directly coupled unit tests.
2. **Given** the replacement commit alone, **When** normative and malformed V3
   vectors are executed, **Then** valid packets pass and invalid packets are
   rejected atomically.
3. **Given** fork-only Mapping/Repair extensions and independent interop
   tooling, **When** commit ownership is inspected, **Then** those concerns
   remain separate from the official V3 protocol commit.

---

### User Story 2 - Preserve the Validated Experimental Result (Priority: P1)

As an NDNSF maintainer, I can continue from `Experimental` after the history
rewrite without losing any validated Spec 113 or Spec 114 behavior.

**Why this priority**: A review-only history cleanup must not silently change
the tested final source tree or discard later reliability work.

**Independent Test**: Compare the rewritten `Experimental` tree with the
frozen pre-rewrite `53dd158` tree and rerun all candidate-bound validation.

**Acceptance Scenarios**:

1. **Given** the frozen pre-rewrite Experimental head, **When** the rewritten
   stack is complete, **Then** the final source tree is byte-for-byte equivalent
   unless an explicitly documented review correction is required.
2. **Given** Spec 113 reliability commits and Spec 114 extension/harness work,
   **When** the stack is replayed, **Then** each remains in a correctly owned,
   independently reviewable commit.
3. **Given** rewritten commit identities, **When** validation is rerun, **Then**
   no evidence is inherited solely from the old OIDs.

---

### User Story 3 - Publish and Recover the Rewritten Review Branch (Priority: P2)

As the fork owner, I can publish the verified history to `origin/master`, see
the complete V3 commit from all required refs, and restore the old remote state
if post-publication verification fails.

**Why this priority**: A destructive remote rewrite without an identity fence,
backup, or receipt could overwrite concurrent reviewer work or leave PR #36 in
an unrecoverable state.

**Independent Test**: From a fresh fetch, verify the expected replacement V3
OID is an ancestor of `Experimental`, local `master`, and `origin/master`, that
local and remote master agree, and that the remote backup still names the old
head.

**Acceptance Scenarios**:

1. **Given** the recorded old remote head, **When** the publication begins,
   **Then** it aborts if `origin/master` no longer equals that expected OID.
2. **Given** all local gates pass, **When** publication is authorized, **Then**
   a remote safety ref is created before a lease-protected update of
   `origin/master`.
3. **Given** the update succeeds, **When** PR #36 and all required refs are
   inspected after a fresh fetch, **Then** they expose the same complete V3
   commit.
4. **Given** a failed post-publication gate, **When** rollback is authorized,
   **Then** the recorded backup restores the previous remote head without
   guessing an OID.

### Edge Cases

- `origin/master` moves after the baseline is recorded but before publication.
- A history conflict produces the right final tree but places V3 behavior in a
  descendant fixup rather than the replacement V3 commit.
- The replacement V3 commit builds at the final head but not when checked out
  alone.
- Replaying `Experimental` duplicates a V3 behavior already folded into the
  replacement commit.
- A remote backup exists locally but was never verified on the remote.
- PR #36 updates to the new head but retains a misleading description or stale
  commit references.
- Commit OIDs change while evidence still names the pre-rewrite candidate.
- A force-with-lease rejection is mistaken for a transient push failure and
  retried without re-auditing the remote change.
- A local Boost 1.71 workaround is accidentally committed into the NDN-SVS
  review history or left reachable through `compileTMP`.
- A local build changes the reviewed branch instead of using a disposable
  `compileTMP` branch rooted at the exact commit being tested.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The rewrite MUST preserve immutable local and remote references to
  the pre-rewrite `master`, `origin/master`, and `Experimental` identities.
- **FR-002**: The rewrite MUST record the expected remote head and MUST refuse
  publication if the remote head changes before the update.
- **FR-003**: One replacement V3 commit MUST contain every official V3
  production-code upgrade and correction currently distributed across the old
  V3 commit and Spec 114 Core fixes.
- **FR-004**: The replacement V3 commit MUST include its directly coupled unit,
  malformed-input, fixed-vector, serial/parallel-equivalence, and validation
  tests so the commit is independently buildable and reviewable.
- **FR-005**: The replacement commit MUST cover V3 naming, embedded signed Data,
  parameters digest, canonical StateVector/BootstrapTime semantics, validation
  ordering, atomic rejection, protocol timing, and zero Sync-Ack behavior.
- **FR-006**: Explicit V2 compatibility and rollback behavior MUST remain
  isolated and testable; no hybrid V2-name/V3-content profile may remain.
- **FR-007**: Fork-only Mapping/Repair extensions MUST NOT be represented as
  official V3 behavior and MUST remain in a separately reviewable commit.
- **FR-008**: Independent NDNts fixtures, peer programs, harnesses, and review
  documentation MAY remain separate, but they MUST validate the replacement V3
  commit rather than compensate for it.
- **FR-009**: The rewritten PR #36 tail MUST not retain a later commit whose sole
  purpose is to repair official V3 behavior missing from the replacement V3
  commit.
- **FR-010**: The rewritten `Experimental` branch MUST preserve all validated
  Spec 113 and Spec 114 non-Core behavior without duplicating folded V3 changes.
- **FR-011**: The rewritten `Experimental` final tree MUST equal the frozen
  pre-rewrite tree, unless a difference is explicitly justified, reviewed, and
  revalidated.
- **FR-012**: The replacement V3 commit MUST pass its focused build and tests
  when checked out in isolation.
- **FR-013**: The final rewritten `Experimental` candidate MUST pass the complete
  unit, standalone interop, MiniNDN, and NDNSF consumer gates required by Spec
  114.
- **FR-014**: Candidate-bound evidence MUST be regenerated against rewritten
  OIDs; old results MAY be retained only as historical evidence.
- **FR-015**: Publication MUST create and verify a remote safety ref before
  updating `origin/master`.
- **FR-016**: Updating `origin/master` MUST use an explicit lease bound to the
  recorded old remote OID; plain force push is prohibited.
- **FR-017**: After publication and a fresh fetch, local `master` and
  `origin/master` MUST resolve to the same head.
- **FR-018**: The replacement V3 commit MUST be an ancestor of `Experimental`,
  local `master`, and `origin/master` after completion.
- **FR-019**: PR #36 MUST point to the new `origin/master` head and its review
  description/evidence MUST identify the replacement V3 commit and the
  standards-compatibility correction.
- **FR-020**: A tested rollback procedure MUST name the exact safety ref,
  expected current remote OID, and lease-protected restoration command.
- **FR-021**: No upstream branch, tag, release, or unrelated local worktree may
  be modified by this feature.
- **FR-022**: All history surgery MUST occur in an isolated worktree or temporary
  branch; the validated current checkout remains recoverable until publication
  succeeds.
- **FR-023**: External mutation of `origin/master` and PR #36 MUST occur only
  after an explicit execution authorization and all pre-publication gates pass.
- **FR-024**: Completion evidence MUST distinguish commit composition, local
  branch state, remote branch state, PR state, executed tests, and measured
  network results.
- **FR-025**: Cross-implementation C++/NDNts peer programs, TypeScript sources,
  dependency locks, standalone launchers, and MiniNDN orchestration MUST be
  owned by NDNSF rather than NDN-SVS. NDN-SVS review branches MUST contain only
  library/production source, normal project examples, deterministic protocol
  fixtures, and unit tests; they MUST NOT track the external interoperability
  harness or its Node dependencies.
- **FR-026**: The complete local range from `upstream/master` through the shared
  `master`/`Experimental` head MUST be consolidated into exactly nine review
  units organized by owning behavior rather than by the chronology of later
  bug discovery. A tests-only review unit is prohibited when its cases can be
  assigned to the production behavior they exercise.
- **FR-027**: A corrective commit MUST be folded into the earliest surviving
  commit that introduced the corrected mechanism whenever the correction has no
  independently useful API, rollback, or acceptance boundary. In particular,
  late piggyback recovery belongs with piggyback Mapping, pending-fetch iterator
  stability belongs with typed piggyback bounds, and duplicate sparse-Mapping
  suppression belongs with sparse Mapping recovery.
- **FR-028**: Segmented publication and segmented recovery MUST be separate
  owners because they implement distinct failure domains and state machines.
  The publication owner covers sequence reservation, packet staging, ordered
  commit, visibility, abort, and Store rollback. The recovery owner covers
  fetch windows, deadlines, retry/backoff, segment validation, Repair, and the
  atomic Mapping/Repair extension transaction. The official V3 owner MUST
  remain separate from fork extensions. A committed Boost 1.71 build-policy
  owner is prohibited.
- **FR-029**: After the full candidate history is constructed, every surviving
  commit MUST pass `git diff --check` and the complete unit suite available for
  its exact tree. Local compilation MUST follow the disposable `compileTMP`
  procedure in FR-032. A failure MUST be repaired in its owning commit and
  validation resumed from that OID; a descendant MUST NOT compensate for a
  broken owner.
- **FR-030**: The final rewritten head tree MUST be byte-identical to the tree
  of frozen local candidate `a965ad6c847ee86c90289ef3bab00a49ae042396`.
  Local `master` and `Experimental` MUST resolve to the same validated
  nine-commit candidate, and `origin/master`, upstream refs, tags, and PR #36
  MUST remain unchanged.
- **FR-031**: Every surviving NDN-SVS review commit MUST require Boost 1.74 in
  tracked `wscript`; the final range MUST contain no committed Boost 1.71
  threshold, message, or fork build-policy commit. This requirement applies
  only to `/home/tianxing/NDN/ndn-svs`; NDNSF and other repositories retain
  their existing Boost 1.71 policy.
- **FR-032**: Every local NDN-SVS compilation of a review commit MUST create a
  temporary branch named `compileTMP` at the exact OID, lower only the temporary
  `wscript` requirement to Boost 1.71, configure/build/test there, switch back
  to the original review branch, and delete `compileTMP`. The temporary change
  MUST NOT be merged, rebased, pushed, or retained after the build.
- **FR-033**: The rewritten local range MUST begin with exactly three focused
  owners in this order: regex/name-only PubSub, V2 `InterestSigner`, and
  complete corrected SVS V3.
- **FR-034**: A dedicated local PR branch MUST point exactly to the corrected
  SVS V3 owner and MUST contain no Mapping optimization, parallel processing,
  asynchronous publication, sparse Mapping recovery, segmented-publication
  transaction, or Fetcher/Repair descendant.
- **FR-035**: The corrected SVS V3 owner MUST fold the currently normative
  SVS-PS V3 naming and signature-coverage corrections into the earliest commit
  that introduces V3 PubSub integration; unsigned trailing MappingData or
  RepairData MUST NOT be presented as standard SVS V3 behavior.
- **FR-036**: The remaining six behavior owners MUST be replayed only on a
  continuation branch descending from the focused PR boundary. They MUST not
  be required to build or test the focused PR branch.
- **FR-037**: The current dirty `Experimental` checkout MUST remain unchanged.
  All construction MUST occur in a new isolated worktree rooted at refreshed
  `upstream/master`, with immutable local safety refs for the old heads.
- **FR-038**: No branch, commit, or tracked file in NDN-SVS may contain Spec Kit,
  GSD, NDNSF planning, audit, campaign, result, or external interop-harness
  artifacts. Such material MUST remain in the NDNSF repository.
- **FR-039**: The final continuation tree MAY differ from the frozen nine-owner
  tree only for the normative SVS-PS V3 correction and directly coupled tests.
  Every differing hunk MUST be recorded and attributed in NDNSF evidence.
- **FR-040**: This feature authorizes local history construction and local
  branch creation only. No push, PR creation/edit, remote ref mutation, or
  review-thread mutation is authorized.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Exactly one replacement V3 commit owns 100% of the official V3
  production behavior and directly coupled tests identified by the ownership
  manifest; zero official V3 repair-only descendant commits remain.
- **SC-002**: The replacement V3 commit passes 100% of its isolated build,
  fixed-vector, malformed-input, and focused unit gates.
- **SC-003**: The rewritten Experimental tree has zero unexplained file or byte
  differences from the frozen `53dd158` tree.
- **SC-004**: The final candidate passes 100% of the required Spec 114 unit,
  standalone interop, MiniNDN, and NDNSF consumer cells without counting stale
  OID-bound evidence.
- **SC-005**: A fresh fetch reports zero divergence between local `master` and
  `origin/master`.
- **SC-006**: One identical replacement V3 OID is reported as an ancestor of all
  three required refs: `Experimental`, `master`, and `origin/master`.
- **SC-007**: The remote safety ref resolves to the exact old `origin/master`
  OID and remains available after publication verification.
- **SC-008**: PR #36 reports the new head OID, remains reviewable, and names the
  corrected standards-compliant V3 commit with no stale controlling OID.
- **SC-009**: Strict Spec Kit structure, traceability, task-cohesion, and
  post-implementation audit checks report zero unresolved Critical or High
  findings.
- **SC-010**: A repository ownership scan finds zero external C++/NDNts harness
  or Node dependency files in final NDN-SVS `master` and `Experimental`, while
  NDNSF-owned `.ts` and C++ peers pass standalone and MiniNDN bidirectional V3
  interoperability against the exact final NDN-SVS candidate.
- **SC-011**: The rewritten `upstream/master..master` range contains exactly
  nine commits, every old change group and test has exactly one declared
  production owner, and zero corrective-only or tests-only descendant commits
  remain.
- **SC-012**: Every surviving commit reports successful `git diff --check` and
  complete unit-suite evidence bound to its exact OID and temporary
  `compileTMP` build receipt; environment-blocked commits are not counted as
  passing.
- **SC-013**: Rewritten `master == Experimental`, their tree ID equals the tree
  ID of the final Boost 1.74 candidate, and `origin/master` remains unchanged
  without any push, PR edit, or other remote mutation.
- **SC-014**: A per-commit scan reports `107400` and `Boost is 1.74.0` for every
  surviving NDN-SVS commit, zero tracked 1.71 build-policy markers, and no local
  `compileTMP` branch after validation.
- **SC-015**: `upstream/master..pr/svs-v3-focused` contains exactly three
  commits in the required order and the third is the single corrected V3 owner.
- **SC-016**: The focused branch diff contains zero files owned solely by
  Mapping/Repair recovery, parallel/async execution, NDNSF planning, external
  interop orchestration, or generated evidence.
- **SC-017**: Each of the three focused commits passes `git diff --check`, its
  available complete unit suite through the disposable `compileTMP` procedure,
  and its behavior-specific tests at the exact OID.
- **SC-018**: The original dirty checkout, all remote refs, PR #36, upstream
  refs, and tags retain their pre-task identities after local construction.

## Assumptions

- `origin` is the user's fork and `origin/master` is the source branch of open
  upstream PR #36.
- The frozen rewrite source identity is `master == Experimental == a965ad6`;
  `origin/master`, upstream, tags, and all worktrees must be refreshed and
  recorded before construction, and no remote mutation is authorized.
- `upstream/master` is read-only for this feature.
- The current `Experimental` tree is the accepted functional source target; the
  feature reorganizes ownership and history rather than redesigning V3, except
  for the explicitly required restoration of the tracked Boost 1.74 baseline.
- The host provides Boost 1.71 for local compilation; lowering the configure
  check is a disposable NDN-SVS validation action, not reviewable source.
- An explicit implementation request authorizes local history construction;
  remote publication still observes FR-023 immediately before mutation.
