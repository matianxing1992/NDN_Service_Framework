# Research: NDN-SVS Experimental Convergence

## Decision 1: Reconstruct from the reviewed baseline instead of merging histories

**Decision**: Pin `origin/master`, preserve the old dirty state on a backup
branch, and replay only descendants of the Experimental creation point onto the
reviewed baseline.

**Rationale**: The histories share intent but not commit identity. A merge dry
run produced multiple conflict files and duplicate behavior. Reconstruction
keeps upstream review decisions and produces a linear review surface.

**Alternatives considered**:

- Ordinary `git merge Experimental`: rejected because it preserves duplicate
  history and conflict-heavy implementations.
- Ordinary `git rebase origin/master`: rejected because it attempts to replay
  eleven commits, including reviewed equivalents.
- Stash then reset: rejected because a stash is too easy to drop and does not
  durably preserve selected ignored artifacts.

## Decision 2: Use `0521665` as the selective replay boundary

**Decision**: Replay the two commits created after `Experimental` branched from
`0521665`, plus the explicit backup snapshot delta.

**Rationale**: Reflog evidence shows `Experimental` was created at `0521665` and
then added `b392d1f` and `5b5461a`. Earlier functionality was subsequently
reviewed and rewritten on remote master.

**Alternatives considered**:

- Merge-base `38f5148`: rejected because it would replay superseded local
  versions of reviewed features.
- Hand-copy every final file: rejected because it loses commit provenance and
  makes accidental scope expansion harder to detect.

## Decision 3: Preserve upstream InterestSigner

**Decision**: Keep the reviewed ndn-cxx `InterestSigner` implementation from
remote master.

**Rationale**: It is the reviewed dependency primitive and replaces the local
custom timestamp workaround. No current requirement justifies restoring the
older implementation.

**Alternatives considered**: retain `d300de4`; rejected as duplicate ownership
and a known merge conflict.

## Decision 4: Treat `publishAsync` as asynchronous advertisement

**Decision**: Preserve its signature but document and test that safe local
preparation happens before return, while network-face work and advertisement
remain on the event loop.

**Rationale**: The API has no completion/error channel. Returning before
fallible preparation creates invisible failures and sequence gaps. Moving Face
operations to arbitrary caller threads is unsafe. The selected contract makes
the unavoidable synchronous portion explicit and keeps advertisement deferred.

**Alternatives considered**:

- Restore fully background preparation: rejected because the existing return
  type cannot communicate a failed reservation safely.
- Add a new future/callback API: rejected as unnecessary scope expansion for
  convergence; it can be designed separately later.
- Make `publishAsync` identical to synchronous publish: rejected because it
  would remove all deferred advertisement behavior.

## Decision 5: Retry a failed pre-commit head without advancing

**Decision**: Before the local version vector changes, the head publication
remains prepared and blocks later sequence advertisement until a bounded
event-loop retry succeeds or shutdown rolls back all unadvertised state. After
the local version vector changes, network Sync send failure is contained and
recovered by later Sync rather than pretending the committed local state was
rolled back.

**Rationale**: Discarding a sequence already returned to a caller or advancing
past it violates sequence continuity. A retained head provides deterministic
ordering and supports injected transient-failure recovery.

**Alternatives considered**:

- Drop and reuse the sequence: rejected because callers may already hold it.
- Skip to later sequences: rejected because it creates unreadable state gaps.
- Infinite busy retry: rejected because it can monopolize the event loop.

## Decision 6: Make rollback support explicit without breaking source compatibility

**Decision**: Add a rollback-capability query with a source-compatible default of
unsupported. Reject every asynchronous transaction that can leave stored but
unadvertised Data before insertion unless the store opts in and implements
`erase(Name)`; this includes a single-packet publication.

**Rationale**: A default no-op cannot provide atomic rollback after partial
insertion. Compile-time rejection is safer than runtime false success.

**Alternatives considered**: make `erase` pure virtual; rejected because it
breaks every existing custom DataStore at compile time even when it never uses
transactional asynchronous publication. A silent default no-op is also
rejected.

## Decision 7: Separate Spec 110 and Spec 112 review concerns

**Decision**: Bounded fetch/repair remains a distinct commit from segmented
atomicity and callback-lifetime fixes.

**Rationale**: Recovery policy/Repair TLVs are not needed to fix the reported
oversize transaction bug and must be independently reviewable and removable.

## Decision 8: Fresh evidence is mandatory

**Decision**: Rebuild and create a new MiniNDN candidate after the final commit
sequence stabilizes.

**Rationale**: Rebase/conflict resolution changes source identity and may change
behavior. Old evidence remains useful history but cannot approve new bytes.
