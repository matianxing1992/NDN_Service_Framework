# Selective Replay Classification

**Pinned baseline**: `db9fc25a5d5c27a44506da343823ef92d085a9c2`  
**Replay boundary**: `0521665da900a5fb208c20fe468540704df92a12`  
**Immutable source snapshot**: `7b619fac123155fd8b17f3cea2d5701085ed6ed2`

## Replay Result

`review/ndn-svs-convergence` replayed exactly three descendants with `--onto`:

| Old | Replayed | Subject | Range-diff |
|---|---|---|---|
| `b392d1f` | `b836d50` | Experiment with partial SVS mapping responses | `=` |
| `5b5461a` | `b64275f` | Suppress duplicate SVS mapping fetches | `=` |
| `7b619fa` | `7fa8300` | WIP recovery snapshot | `=` |

The selective rebase completed without textual conflicts. Therefore no manual
conflict resolution was needed in mapping, security, publication, or tests.
`git range-diff` nevertheless proves the three replayed patches are equivalent.

## Excluded Reviewed-Equivalent History

Nine pre-boundary local commits (`83a0b47` through `0521665`) were not replayed.
Their reviewed remote sequence is `d0e128d`, `b99a66c`, `0b56a94`, `627b273`,
`7433913`, `1b1ff2c`, `b7a8e68`, and `db9fc25`. The local custom timestamp commit
`d300de4` is specifically replaced by reviewed remote commit `627b273`.

The pre-reset replay tree differed from the immutable backup tree only in:

```text
ndn-svs/security-options.cpp
ndn-svs/security-options.hpp
tests/unit-tests/security-options.t.cpp
```

Those three differences are the reviewed ndn-cxx `InterestSigner` version and
tests. There is zero security-options delta against `origin/master`, and current
source contains `security::InterestSigner::makeSignedInterest`.

## WIP Conversion And Content Accounting

After replay verification, `git reset --mixed origin/master` was applied only to
the review branch, exposing the complete retained product delta without WIP
commits. The permanent backup was not moved.

Retained review concerns:

- sparse/partial mapping and duplicate fetch suppression;
- bounded publication fetch/repair and Repair TLVs;
- segmented transactional publication, DataStore, and Fetcher lifecycle work;
- local Boost 1.71 configure policy.

Dropped only from product history, while retained in the permanent backup:

- `docs/high-loss-repair-design.md`;
- product `.gitignore` additions for local analysis/spec/docs/results.

The local ignore rules were moved to `.git/info/exclude`. `.gitignore` and the
reviewed security files now have zero delta from `origin/master`. No source
content was lost: product behavior remains in the uncommitted review surface,
while local-only documentation remains recoverable from the backup and present
as ignored local files.
