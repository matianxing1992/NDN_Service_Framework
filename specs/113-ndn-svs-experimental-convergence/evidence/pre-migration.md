# Pre-Migration Evidence

**Captured**: 2026-07-15T22:31:14-05:00  
**Target repository**: `/home/tianxing/NDN/ndn-svs`  
**Rule**: no fetch or remote mutation occurred during capture.

## Refs And Topology

| Ref | OID / state |
|---|---|
| `Experimental` | `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` |
| local `master` | `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` |
| pinned `origin/master` | `db9fc25a5d5c27a44506da343823ef92d085a9c2` |
| `origin/Experimental` | absent |
| merge base: Experimental vs origin/master | `38f514807c3dacd20f4dd3237c061cb6d3883d3a` |

`master` tracks `origin/master` and was 11 commits ahead / 8 behind.
`Experimental` has no upstream. The Experimental reflog records creation from
`0521665da900a5fb208c20fe468540704df92a12`, then `b392d1f`, then `5b5461a`.

Origin URL is `git@github.com:matianxing1992/ndn-svs.git`. Captured origin refs:

```text
origin/HEAD=db9fc25a5d5c27a44506da343823ef92d085a9c2
origin/develop=d67b4d51f32b72228e53d8026e3fdcbfd7d584eb
origin/mapping-time=55ddd210e88f249d85cd66a2a8626e663c3c38d7
origin/master=db9fc25a5d5c27a44506da343823ef92d085a9c2
origin/spec110-boost171=19ec38ec77d26c13125b292863e607da51a3d9de
origin/spec110-runtime-publication-fetch=7b616b08624a79617bb05f2d3553bbbacdc4c482
origin/spec110-sealed-5b5461a72801=5b5461a728012e9d0959e99ef0acbc5f32fc9d25
origin/svsv2=ad7281108716e4d7da45154410253075d19bc388
```

## Worktree Manifest

- Porcelain-status SHA-256:
  `df98e842617216fab29cf147991e876060eaaba6982590250a87ad1f1d22a34f`
- Binary tracked-diff SHA-256:
  `ef937dac2a2324c8e0ebdd66a5e77e8269014a63428279b276ae8d008aa328fd`

Tracked modifications:

```text
.gitignore
ndn-svs/fetcher.cpp
ndn-svs/fetcher.hpp
ndn-svs/mapping-provider.hpp
ndn-svs/store-memory.hpp
ndn-svs/store.hpp
ndn-svs/svspubsub.cpp
ndn-svs/svspubsub.hpp
ndn-svs/svsync-base.cpp
ndn-svs/svsync-base.hpp
ndn-svs/tlv.hpp
tests/unit-tests/mapping-provider.t.cpp
tests/unit-tests/svspubsub.t.cpp
wscript
```

Explicitly selected ignored recovery artifacts:

| Path | SHA-256 | Ignore source |
|---|---|---|
| `docs/high-loss-repair-design.md` | `564797c7a7b3fc91661912a6eda7ce3b7bb169b1c470f4f2f3e8dfe8a98781c6` | `.gitignore:45` |

## Credential Scan

The 14 tracked modified paths plus the two selected ignored artifacts were
scanned by filename without printing matching content for PAT, private-key,
password, token, and secret assignment patterns. Result: `MATCH_FILE_COUNT=0`.
No authentication configuration, credential file, result tree, or other ignored
path is approved for the snapshot.

## Stop Condition

Before any final ref move, all captured origin ref OIDs—especially pinned
`origin/master`—must be identical. External drift stops Spec 113.
