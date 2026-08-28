# Research: Complete SVS V3 Review Commit

## Decision 1: Rewrite the owning V3 commit

**Decision**: Replace the existing incomplete V3 commit in the open PR history
and fold all official V3 Core corrections and direct tests into that commit.

**Rationale**: PR #36 is open and marked changes requested. One reviewer
demonstrated `/v=2` plus raw StateVector while normative V3 requires `/v=3`
plus embedded Data; another explicitly requested that later repairs be folded
into the commit that introduced the code. A descendant fix would preserve the
review defect.

**Alternatives considered**:

- Append `fix: make V3 compliant`: rejected because the owning commit remains
  independently invalid and difficult to review or bisect.
- Leave origin unchanged and create only a new feature branch: rejected because
  the user requires `origin/master` and PR #36 to expose the complete commit.

## Decision 2: Define “all V3” as official protocol behavior

**Decision**: The complete V3 commit includes official wire/profile behavior
and directly coupled tests, but excludes fork-only Mapping/Repair extensions,
publication recovery, and independent interop tooling.

**Rationale**: Those mechanisms have separate consumers and are not part of the
SVS V3 normative protocol. Folding them into the V3 commit would make review
larger while falsely presenting extensions as standard behavior.

**Alternatives considered**:

- Fold every Spec 113/114 change into one commit: rejected as semantically
  inaccurate and non-revertible.
- Keep validation atomicity in a final fix commit: rejected because it is a V3
  correctness invariant and belongs with the codec.

## Decision 3: Preserve final Experimental tree identity

**Decision**: Rebase/reconstruct Experimental so its final tree equals the
current validated `53dd158` tree, while removing V3 code duplicated by the new
master ancestor.

**Rationale**: This separates history cleanup from functional redesign and
provides a strong, cheap completeness oracle before expensive reruns.

**Alternatives considered**:

- Accept arbitrary tree drift if tests pass: rejected because tests may miss
  lost extension or operational behavior.
- Point Experimental directly at rewritten master: rejected because it would
  discard validated Spec 113/114 work that is intentionally outside PR #36.

## Decision 4: Use exact remote fencing and durable backup

**Decision**: Create a verified remote backup and update `origin/master` only
with an explicit expected-old-OID force-with-lease.

**Rationale**: PR reviewers may update or depend on the branch during local
rewrite work. An explicit OID is a fencing token against overwriting a remote
change. A remote backup allows recovery even if the local workspace is lost.

**Alternatives considered**:

- Plain `--force`: prohibited because it ignores concurrent updates.
- Generic `--force-with-lease`: rejected because implicit tracking state may be
  stale or refreshed unexpectedly.
- Local backup only: rejected because publication is an external-state change.

## Decision 5: Revalidate rewritten identities

**Decision**: Run isolated replacement-commit tests and the complete final
candidate gates again; old Spec 114 results remain historical only.

**Rationale**: A rewritten commit can fail at its own point even when the final
tree passes. Candidate manifests and evidence are identity-bound, so new OIDs
need new receipts.

**Alternatives considered**:

- Trust tree equality alone: rejected because the replacement commit's isolated
  buildability and PR-tail ordering are separate properties.
- Rerun only unit tests: rejected because the original defect was exposed by an
  independent NDNts wire peer.

## Decision 6: Treat PR metadata as part of publication

**Decision**: After the remote rewrite, verify and update PR #36 description and
evidence references so the corrected commit is discoverable to reviewers.

**Rationale**: A technically correct branch with stale review instructions and
OIDs still imposes avoidable reviewer ambiguity.

**Alternatives considered**:

- Let the commit list update silently: rejected because the controlling review
  finding and new evidence should be explicitly connected.

## Decision 7: Fold corrective-only descendants into behavior owners

**Decision**: Consolidate the local review range into exactly nine commits. A later
bug fix is folded into the commit that introduced the mechanism unless it has a
separate API, rollback boundary, or independently meaningful acceptance gate.

**Rationale**: Reviewers should see the correct mechanism when they inspect or
bisect its owning commit. Separate sparse-Mapping and duplicate-suppression
commits, or separate transport and policy halves of one extension transaction,
expose development chronology rather than stable design boundaries. Official
V3 remains separate from fork extensions. The former Boost 1.71 commit is not a
review behavior and is therefore excluded.

**Alternatives considered**:

- Keep all 17 commits because each currently passes: rejected because passing
  mechanics do not make corrective-only commits independently meaningful.
- Squash everything into one commit: rejected because it destroys standards,
  API, performance, build-policy, and fork-extension review boundaries.
- Retain a focused tests-only commit: rejected because its three cases map
  directly to the async-publication and piggyback production owners. The
  history rewrite may create the shared test file earlier rather than expose
  development chronology as a review boundary.

## Decision 9: Separate segmented publication from segmented recovery

**Decision**: Split the old combined segmented-publication/recovery commit into
one producer-side failure-atomic publication owner and one receiver-side
bounded fetch/Repair owner. Fold structural Mapping/Repair extension
atomicity into the recovery owner, while moving V2 profile propagation into
the V3 owner.

**Rationale**: The producer operates sequence reservation, staged packet
batches, ordered commit, visibility, and Store rollback. The receiver operates
fetch windows, deadlines, retry/backoff, segment assembly, validation, and
Repair. They share publication identity but not an atomic state machine,
failure transition, or rollback surface. Separate commits make each failure
domain independently buildable, testable, and revertible.

**Alternatives considered**:

- Keep the 2,200-line combined commit: rejected because it hides two distinct
  state machines and prevents focused review of publication atomicity versus
  network recovery.
- Keep extension validation as a final fixup: rejected because Repair is
  introduced by the recovery mechanism and its structural transaction belongs
  in that owner.

## Decision 8: Separate review policy from local Boost availability

**Decision**: Every tracked NDN-SVS review commit retains Boost 1.74. Local
Boost 1.71 builds use a newly created `compileTMP` branch at the exact reviewed
OID; the temporary threshold change is discarded with that branch after tests.

**Rationale**: Reviewers must see the intended upstream dependency boundary,
while the Ubuntu 20.04 host still needs a bounded way to compile each tree.
Making the workaround disposable satisfies both without claiming that NDN-SVS
supports Boost 1.71 upstream. This policy is intentionally limited to NDN-SVS;
NDNSF and other local repositories keep their existing Boost 1.71 baselines.

**Alternatives considered**:

- Keep a Boost 1.71 commit in the PR: rejected because it changes the reviewed
  dependency policy for a local environment limitation.
- Edit the reviewed branch before each build: rejected because it contaminates
  exact-OID evidence and risks accidental publication.
- Install a second local Boost 1.74 tree: not required by the user; the
  disposable branch gives a reproducible local gate without changing history.
