# Publication and Recovery Receipt

> Historical receipt for published head `0c09d65` and the later local
> `6b1fc52` correction. Current local `master == Experimental == ee4e174` is
> intentionally unpushed and documented in
> `boost174-compiletmp-validation.md`.

## Current state

| Ref | OID | State |
|---|---|---|
| local `master` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` | corrected, locally validated nine-commit head |
| local `Experimental` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` | synchronized with local master |
| local V3 owner | `9f679bdb1bbaaa0c351c3179fa25beaef881ef84` | ancestor of both local candidate refs |
| `origin/master` | `0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` | first publication; superseded locally by CI fix |
| PR #36 head | `0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` | 25/31 CI jobs pass; six old-compiler jobs fail |

The first publication preserved old remote master on
`safety/spec115-pre-final-publication-20260717T004458Z`, independently verified
that backup, refreshed the remote fence, and changed only remote master with an
explicit old-OID lease. Upstream master, tags, releases, and any remote
Experimental ref were not changed. GitHub Actions then exposed a missing direct
`<atomic>` include. That correction is folded into its semantic owner, and local master
and Experimental now intentionally differ from origin/PR until the user
authorizes another push.

## Local rollback

```text
safety/pr36-message-master-20260716T213331Z
  -> c62f4b50cac742628a84575fa4f61dc93f9b14cb
safety/pr36-message-experimental-20260716T213331Z
  -> b1c3f49254726bc117edda9431dd4c4b58b4f646
safety/spec115-ndnsf-interop-ownership-20260716T2310Z
  -> 7fd50f9951de75fc04d67a7c036f86bb5ec9a941
safety/spec115-experimental-review-20260716T230617Z
  -> 70e682f8500e2ad205ec17722287ea0b8bd6a9f0
```

These safety refs preserve the exact validated source trees before the
message-only rewrite. Earlier remote backup receipts remain valid historical
recovery points for the public PR head.

## Final publication and rollback

Publication command:

```text
git push origin \
  --force-with-lease=refs/heads/master:2b052c94444044cb34eb4160b6e76d6564c7c918 \
  0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f:refs/heads/master
```

Verified remote backup:

```text
safety/spec115-pre-final-publication-20260717T004458Z
  -> 2b052c94444044cb34eb4160b6e76d6564c7c918
```

Lease-protected rollback:

```text
git push origin \
  --force-with-lease=refs/heads/master:0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f \
  2b052c94444044cb34eb4160b6e76d6564c7c918:refs/heads/master
```

This receipt is historical proof of the first fenced publication, not proof
that corrected candidate `6b1fc52` is published. T033 was reopened after the CI
repair. No new push is authorized; future publication must first preserve and
lease against the then-current remote head, expected to be `0c09d65`.

## Corrected candidate held locally

```text
owner 11:          b276ec1e3a074031ca8ca105ca3e99406f57e6ec
candidate head:    6b1fc525e31f6d79e337d40164dfb569f18967ed
candidate tree:    40d6d82a3a2115110e1011291f79a75c36180500
local safety ref:  safety/spec115-ci-old-head-20260717T004955Z -> 0c09d65...
```

The candidate passed Boost 1.71 owner/final unit gates, standalone 5/5, and
MiniNDN 6/6. It remains local by explicit user instruction. This receipt does
not contain or authorize a new push command.
