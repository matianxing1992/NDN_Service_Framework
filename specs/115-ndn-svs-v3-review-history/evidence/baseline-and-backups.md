# Spec 115 Baseline and Backups

> Historical baseline and safety-ref receipt. Current local candidate identity
> is `ee4e174`; no remote safety identity below was mutated by the final local
> nine-owner rewrite.

Captured: `2026-07-16T09:57:20Z`

## Ref identities after fresh fetch

| Authority | OID | Tree / state |
|---|---|---|
| `upstream/master` | `a93724758aca71a4ea327574ef7af46770a81a40` | read-only base |
| local `master` | `db9fc25a5d5c27a44506da343823ef92d085a9c2` | tree `a2d3cc01841c7851fdac18929d4b0f5a3848cc1b` |
| `origin/master` | `db9fc25a5d5c27a44506da343823ef92d085a9c2` | publication fence |
| `Experimental` | `53dd1588201b967a4aa9decd3e51ade3263e0f88` | tree `be3d9ccd348377160ab7afffabcbdea9459cff4f` |
| PR #36 head | `db9fc25a5d5c27a44506da343823ef92d085a9c2` | open, mergeable, changes requested |

PR: <https://github.com/named-data/ndn-svs/pull/36>

## Immutable local safety refs

- `backup/spec115-master-pre-rewrite-20260716T095720Z` ->
  `db9fc25a5d5c27a44506da343823ef92d085a9c2`
- `backup/spec115-experimental-pre-rewrite-20260716T095720Z` ->
  `53dd1588201b967a4aa9decd3e51ade3263e0f88`

Both refs were created before moving any live branch. They remain retained
through feature completion. The remote backup is intentionally deferred to the
publication gate, where the remote fence is refreshed immediately beforehand.

## Worktree ownership

- `/home/tianxing/NDN/ndn-svs`: clean, owns `Experimental`.
- `/home/tianxing/NDN/ndn-svs-pr36-review`: owns
  `pr36-review-clean` at the old master head.
- `/home/tianxing/NDN/ndn-svs-pr36-perpatch`: detached historical review tree.
- `/tmp/ndn-svs_e3be_probe`: detached historical probe.
- `/tmp/spec110-ndn-svs-boost171`: owns the Spec 110 Boost branch.
- `/tmp/spec110-ndn-svs-runtime`: owns the Spec 110 runtime branch.

No existing worktree will be reused for Spec 115 history construction. The
service-framework control repository already contains unrelated user changes;
Spec 115 edits remain confined to its feature directory, its dedicated helper
and tests, and later candidate-bound Spec 114 evidence.

## Abort conditions

Publication stops without retry when any of these is true:

- refreshed `origin/master` is not the recorded expected old OID;
- PR #36 head and `origin/master` disagree;
- the remote backup cannot be independently resolved;
- isolated or full candidate validation is incomplete;
- target worktree ownership changes unexpectedly;
- explicit publication authorization is absent or withdrawn.

## Rollback authority

The exact pre-publication remote OID is
`db9fc25a5d5c27a44506da343823ef92d085a9c2`. The final rollback command is not
constructed until the new candidate OID and verified remote backup name exist;
it will use an explicit candidate-to-old lease rather than an unconditional
force update.

## Publication-gate refresh and remote backup

Immediately before publication, fresh fetches confirmed:

- `origin/master = db9fc25a5d5c27a44506da343823ef92d085a9c2`;
- PR #36 head is the same OID, OPEN, MERGEABLE, CHANGES_REQUESTED;
- `upstream/master = a93724758aca71a4ea327574ef7af46770a81a40`;
- candidate master `9a6cf1b3909430960afd439a8ddc32773e72da1b`;
- candidate Experimental `fd99b3f51bdd048c783a023f1082a7091a27bac9`.

Remote backup creation and independent query returned:

```text
db9fc25a5d5c27a44506da343823ef92d085a9c2  refs/heads/backup/spec115-master-pre-v3-rewrite-20260716T1050Z
```

Prepared and verifier-accepted rollback command:

```text
git push \
  --force-with-lease=refs/heads/master:9a6cf1b3909430960afd439a8ddc32773e72da1b \
  origin \
  db9fc25a5d5c27a44506da343823ef92d085a9c2:refs/heads/master
```

No live local branch had moved at this point.

## Final consolidated-history publication

Captured `2026-07-17T00:44:58Z`. A fresh independent remote query immediately
before mutation proved that `refs/heads/master` still equaled the required
fence:

```text
2b052c94444044cb34eb4160b6e76d6564c7c918  refs/heads/master
```

The command verifier accepted only the exact destination candidate and old-OID
lease. A new remote safety branch was published first and independently
resolved before master was changed:

```text
2b052c94444044cb34eb4160b6e76d6564c7c918  refs/heads/safety/spec115-pre-final-publication-20260717T004458Z
```

The only live remote update was:

```text
git push origin \
  --force-with-lease=refs/heads/master:2b052c94444044cb34eb4160b6e76d6564c7c918 \
  0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f:refs/heads/master
```

Post-publication fetch and independent remote queries proved:

```text
local master        0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
local Experimental  0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
origin/master       0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
remote master       0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
upstream/master     a93724758aca71a4ea327574ef7af46770a81a40
official V3 owner   5910614a8ea414ca2d9e1cece82ec3098ba1afe5
```

The V3 owner is an ancestor of all three active candidate refs. Upstream
master remained unchanged, both local and upstream tag sets remained empty,
and the upstream repository still had no releases. PR #36 immediately reported
the exact new head and remained open and mergeable. GitHub Actions and review
metadata are the separate final T034 gate.

Exact lease-protected rollback command:

```text
git push origin \
  --force-with-lease=refs/heads/master:0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f \
  2b052c94444044cb34eb4160b6e76d6564c7c918:refs/heads/master
```

## Nine-owner local rewrite baseline

Captured `2026-07-17T02:04:44Z` after fresh read-only fetches:

```text
local master        a965ad6c847ee86c90289ef3bab00a49ae042396
local Experimental  a965ad6c847ee86c90289ef3bab00a49ae042396
origin/master       0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
upstream/master     a93724758aca71a4ea327574ef7af46770a81a40
source tree         7b7acd4581caf20da81b6c628d7ca18497a75d9e
```

New immutable local safety refs retain the three mutable source identities:

```text
safety/spec115-nine-master-20260717T020444Z
safety/spec115-nine-experimental-20260717T020444Z
safety/spec115-nine-origin-20260717T020444Z
```

Construction is isolated at:

```text
branch   spec115/nine-owner-20260717T020444Z
worktree /tmp/ndn-svs-spec115-nine-20260717T020444Z
```

No local live ref, remote ref, PR, upstream ref, or tag was changed by this
baseline step.

## Nine-owner local rewrite completion

After exact-OID validation, local refs were moved together without pushing:

```text
local master        ee4e174b0e5f188f4e48608c60898649c8021bc9
local Experimental  ee4e174b0e5f188f4e48608c60898649c8021bc9
origin/master       0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
upstream/master     a93724758aca71a4ea327574ef7af46770a81a40
final tree          7b7acd4581caf20da81b6c628d7ca18497a75d9e
source tree         7b7acd4581caf20da81b6c628d7ca18497a75d9e
compileTMP branches 0
```

The successful isolated construction branch is
`spec115/nine-owner-20260717T020444Z-round3` in worktree
`/tmp/ndn-svs-spec115-nine-20260717T020444Z-round2`. Earlier round worktrees are
diagnostic artifacts only and are not candidate authorities. No remote ref,
PR body, upstream ref, tag, or release was changed.
