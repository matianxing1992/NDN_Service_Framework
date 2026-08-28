# Quickstart: Complete SVS V3 Review Commit

This guide describes the validation order. It is not authorization to mutate a
remote branch.

## 1. Refresh and freeze identities

From `/home/tianxing/NDN/ndn-svs`:

```bash
git fetch origin --prune
git fetch upstream --prune
git status --short --branch
git rev-parse upstream/master master origin/master Experimental
gh pr view 36 --repo named-data/ndn-svs \
  --json state,headRefOid,baseRefOid,reviewDecision,url
```

Record results in `evidence/baseline-and-backups.md`. Implementation must use
the freshly observed OIDs, not values copied from this design.

## 2. Construct only in an isolated worktree

Create a uniquely named local safety ref and rewrite worktree. Build the
replacement V3 commit according to `contracts/commit-composition.md`. Do not
move local master or Experimental while the candidate is incomplete.

## 3. Validate locally through `compileTMP`

The reviewed NDN-SVS commit must keep Boost 1.74. For every local compile,
create the disposable branch at the exact OID, apply only the temporary 1.71
threshold patch, and remove the branch after returning:

```bash
source_branch=$(git branch --show-current)
test -n "$source_branch"
test -z "$(git branch --list compileTMP)"
cleanup_compile_tmp() {
  git cherry-pick --abort >/dev/null 2>&1 || true
  git switch "$source_branch" >/dev/null 2>&1 || true
  git branch -D compileTMP >/dev/null 2>&1 || true
}
trap cleanup_compile_tmp EXIT
git switch -c compileTMP <exact-reviewed-oid>
git cherry-pick <temporary-boost-1.71-patch-oid>
./waf distclean || true
./waf configure --with-tests
./waf build -j4
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests --log_level=test_suite
git switch "$source_branch"
git branch -D compileTMP
trap - EXIT
```

Before accepting the result, verify `compileTMP` no longer exists and the
reviewed commit still reports `107400` and `Boost is 1.74.0` in `wscript`.
This procedure applies only to NDN-SVS; do not change NDNSF's Boost 1.71
baseline. Do not compile NDN-SVS directly on `master`, `Experimental`, or a
review branch.

## 4. Validate the replacement commit alone

Check out the replacement commit in a clean validation worktree. Configure,
build, and run its focused unit/fixed-vector gates. Confirm packet shape with an
independent decoder. A passing final descendant does not satisfy this gate.

## 5. Replay Experimental and compare trees

Reapply separately owned Spec 113 reliability, fork extension, and interop
commits without duplicating folded V3 code. The principal preservation check is:

```bash
git diff --exit-code <old-experimental-oid>^{tree} \
  <candidate-experimental-oid>^{tree}
```

Any difference requires an explicit path-level explanation before full tests.

## 6. Run candidate-bound full gates

Follow `contracts/validation-evidence.md` and Spec 114's canonical commands.
Create new manifests/results for the rewritten OIDs. Preserve failures as
measured outcomes and do not reuse old candidate receipts.

## 7. Pre-publication review

Verify all of the following before requesting publication authorization:

```bash
git merge-base --is-ancestor <replacement-v3-oid> <candidate-master-oid>
git merge-base --is-ancestor <replacement-v3-oid> <candidate-experimental-oid>
git log --oneline --reverse upstream/master..<candidate-master-oid>
git diff --check upstream/master..<candidate-master-oid>
```

Review the ownership manifest and exact rollback recipe.

## 8. Publish and verify

Only after explicit authorization, follow
`contracts/ref-publication.md`: create and verify the remote backup, recheck the
expected old head, perform the exact-OID lease-protected update, fetch again,
and verify branch/PR identities. Never substitute plain force push.

## Expected completion

- one complete official V3 commit;
- local `master == origin/master == PR #36 head`;
- that V3 OID is an ancestor of `master`, `origin/master`, and `Experimental`;
- Experimental retains the validated final source tree;
- the remote safety ref and exact rollback command remain available.
