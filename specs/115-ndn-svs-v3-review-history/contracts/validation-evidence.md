# Contract: Validation and Evidence

## Gate A: Baseline and ownership

- refreshed local/remote/PR OIDs;
- clean target checkout and worktree ownership inventory;
- complete commit-ownership manifest;
- verified local and remote safety refs.

## Gate B: Replacement V3 commit in isolation

- clean configure/build at the replacement commit;
- V3 fixed vectors and malformed-vector tests;
- Core, VersionVector, timer, serial/parallel, and security validation tests;
- independent packet inspection confirming V3 name and embedded Data.

## Gate C: Rewritten PR tail

- every commit is independently buildable or has an explicit test-only
  dependency on its immediate parent;
- no official V3 repair-only descendant;
- no unrelated path or source-tree drift;
- diff-check and commit-message/ownership review pass.

## Gate D: Replayed Experimental candidate

- old/new Experimental tree comparison;
- duplicate/lost hunk audit;
- complete NDN-SVS units;
- standalone C++/NDNts interop;
- immutable MiniNDN matrix;
- NDNSF consumer regression.

## Gate E: Remote receipt

- fresh-fetch local/remote master equality;
- replacement V3 ancestry for all three required refs;
- remote backup identity;
- PR #36 head and review-description identity;
- rollback command bound to exact OIDs.

## Evidence classification

Use `implemented`, `wired`, `executed`, and `measured` precisely. History or
source inspection is not execution evidence. Old OID-bound results may explain
the baseline but cannot close Gate B-D for rewritten candidates.

## Failure handling

Stop at the current gate, record the failure, repair locally, and continue from
the failing focused gate. Do not restart expensive network matrices after every
local fix; rerun the complete matrix once after all focused gates pass.
