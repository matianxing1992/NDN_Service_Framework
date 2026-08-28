# Backup Verification

**Permanent branch**: `backup/experimental-before-convergence-20260715`  
**Snapshot OID**: `7b619fac123155fd8b17f3cea2d5701085ed6ed2`  
**Snapshot parent**: `5b5461a728012e9d0959e99ef0acbc5f32fc9d25`

## Snapshot Accounting

- Snapshot parent equals the captured original Experimental HEAD.
- The snapshot contains all 14 tracked modified paths and exactly the two
  selected ignored recovery artifacts.
- Tracked-only snapshot diff SHA-256 is
  `ef937dac2a2324c8e0ebdd66a5e77e8269014a63428279b276ae8d008aa328fd`,
  exactly matching the captured worktree diff.
- Snapshot `docs/high-loss-repair-design.md` SHA-256 is
  `564797c7a7b3fc91661912a6eda7ce3b7bb169b1c470f4f2f3e8dfe8a98781c6`.
- A commit-level sensitive-pattern path scan returned zero matching files.
- The backup worktree was clean immediately after the snapshot commit.

## Executed Recovery Drill

A detached disposable worktree at `/tmp/spec113-backup-recovery-7b619fa` was
created from the permanent branch. It proved the snapshot OID and parent,
recomputed both selected artifact hashes, and had a clean status. The disposable
worktree was then removed normally. Result: `RECOVERY_DRILL=PASS`.

## Permanent Retention Contract

Spec 113 will not rebase, reset, delete, rename, or force-move
`backup/experimental-before-convergence-20260715`. Every later mixed reset and
branch-label move is restricted to the review branch, local `master`, or local
`Experimental`. Recovery is:

```bash
git worktree add --detach /tmp/spec113-recover \
  backup/experimental-before-convergence-20260715
```

No stash or reflog entry is needed.
