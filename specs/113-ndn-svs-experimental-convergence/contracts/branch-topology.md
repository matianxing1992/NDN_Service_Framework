# Contract: Branch Topology And Recovery

## Pre-Migration

- Capture local and remote refs without fetching after the baseline decision.
- Record status and a binary-safe tracked diff.
- Record selected ignored local design files explicitly.
- Verify that no credential or authentication file is included.

## Recovery Point

- A permanent backup branch must name the snapshot commit.
- The snapshot parent must be the original Experimental HEAD.
- Recovery must not depend on reflog expiry or a stash entry.
- The backup branch is never rebased, reset, deleted, or force-moved by Spec 113.

## Final Local Topology

```text
backup/experimental-before-convergence-* -> original HEAD + WIP snapshot
master                                    -> pinned origin/master OID
origin/master                             -> unchanged pinned OID
Experimental                              -> pinned OID + cleaned commits
```

`origin/master` must be an ancestor of `Experimental`. Local master and
`origin/master` must be identical. Experimental must be clean.

## Remote Safety

- No `git push`, `git push --force`, remote merge, or remote branch deletion.
- Capture remote refs before and after and compare.
- If the remote changes externally, stop and record baseline drift; do not
  silently update or rebase.

## Replay Boundary

Only commits after `0521665` and the explicit snapshot delta are candidates for
replay. A range-diff/cherry analysis must classify each retained change against
the reviewed baseline.
