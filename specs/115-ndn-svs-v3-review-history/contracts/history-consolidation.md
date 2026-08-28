# Contract: Local Review-History Consolidation

## Frozen inputs

- base: `upstream/master@a93724758aca71a4ea327574ef7af46770a81a40`;
- source head: `master == Experimental ==
  a965ad6c847ee86c90289ef3bab00a49ae042396`;
- remote fence: refresh and record `origin/master` before construction;
- source range: the current 11-commit local review range;
- preservation oracle: source-head tree ID.

Implementation MUST refresh these identities before mutation and abort if the
local source refs or remote fence differ.

## Required ownership sequence

| Target owner | Source commits | Boundary |
|---|---|---|
| Pub/sub selection and name-only publication APIs | `d4301b1` | Public API |
| Bounded piggyback Mapping/Data delivery | `9e9d4e8`, `70de9ff`, piggyback cases from `6a8eb83` | One delivery/cache lifecycle |
| Parallel Sync receive and local batching | `2bd49fc` | Receive-side worker pipeline |
| Parallel production and ordered async publication | `5908110`, async case from `6a8eb83` | Production API and worker pipeline |
| V2 Signed Interest encoding | `5b30e1a` | Independent V2 security migration |
| Standards-compliant V3 synchronization | `e8b360e`, V2-profile propagation from `a965ad6` | Official protocol owner; no fork policy |
| Sparse Mapping recovery without duplicate fetches | `be731fb` | Mapping query/retry lifecycle |
| Failure-atomic segmented publication | producer/storage portions of `7e2ab35` | Reservation/staging/commit/rollback state machine |
| Bounded segmented fetch and Repair recovery | receiver/recovery portions of `7e2ab35`, extension portions of `a965ad6` | Fetch/validation/Repair state machine |

The final sequence MUST contain exactly these nine owners. Tests MUST be folded
into the production owner they exercise. A change may move between the last two
owners only when exact source inspection proves whether it participates in the
producer commit transaction or the receiver recovery transaction.

## Construction and validation rules

1. Create immutable local safety refs before constructing in a unique worktree.
2. Do not move live refs during construction.
3. Preserve final tree identity exactly; no functional redesign is allowed.
4. Validate after the complete history exists, in commit order.
5. Bind diff-check, configure/build, and complete unit results to exact OIDs.
6. Repair a failing owner in place; do not append a corrective descendant.
7. Move local `master` and `Experimental` together only after all gates pass.
8. Do not push, edit PR #36, mutate upstream, or change tags.
9. Every tracked NDN-SVS commit must retain the Boost 1.74 threshold; the old
   Boost 1.71 build-policy commit is excluded from the review sequence.
10. Every local compilation must occur on a disposable `compileTMP` branch
    rooted at the exact reviewed OID. Apply the 1.71 threshold only there,
    compile/test, return to the candidate branch, and delete `compileTMP`.
11. The `compileTMP` branch and its temporary commit must never be pushed,
    merged, rebased into the candidate, or retained after validation.
12. Validation must abort rather than overwrite a pre-existing `compileTMP`
    branch, and must install cleanup that returns to the source branch and
    removes `compileTMP` after either success or failure.
