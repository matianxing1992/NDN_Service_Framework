# Full Local Review-History Consolidation Evidence

> Historical construction receipt for the superseded 12-commit candidate. The
> final nine-commit Boost 1.74 sequence and exact-OID units are recorded in
> `boost174-compiletmp-validation.md`.

## Frozen baseline

Captured 2026-07-16 UTC before any live-ref movement.

| Identity | OID |
|---|---|
| `upstream/master` | `a93724758aca71a4ea327574ef7af46770a81a40` |
| `origin/master` tracking ref | `2b052c94444044cb34eb4160b6e76d6564c7c918` |
| remote `refs/heads/master` from `git ls-remote` | `2b052c94444044cb34eb4160b6e76d6564c7c918` |
| local `master` | `8335643f81be8fe3d49cf6e773569a21762049c9` |
| local `Experimental` | `8335643f81be8fe3d49cf6e773569a21762049c9` |
| frozen source tree | `fc5743665da77667930850969a1c05489733c8f4` |

The range `upstream/master..master` contains exactly 17 commits. The ownership
manifest in `contracts/history-consolidation.md` maps all 17 source OIDs exactly
once into 12 target owners (`missing=[]`, `extra=[]`, `duplicate=0`).

## Recovery and construction refs

| Purpose | Local ref/path |
|---|---|
| frozen source | `safety/spec115-master17-pre-consolidation-20260716T001503Z` |
| frozen remote-tracking identity | `safety/spec115-origin-pre-consolidation-20260716T001503Z` |
| candidate branch | `spec115/consolidate-master17-20260716T001503Z` |
| isolated worktree | `/tmp/ndn-svs-spec115-consolidate-20260716T001503Z` |

Both safety refs resolved to their required exact OIDs after creation. The
candidate branch started at read-only `upstream/master`; neither live local
branch nor any remote ref was moved.

## Pre-implementation audit

- strict Spec Kit structure: PASS (30 FRs, 13 SCs, 31 tasks, T029-T031 open);
- code/history reality: PASS (17 source commits, 12 complete owners);
- final-tree oracle: present;
- remote fence: current and independently observed;
- task cohesion: PASS (freeze/audit, construction, and exact-OID validation are
  three distinct risk and acceptance boundaries);
- verdict: PASS for local history construction; remote publication is outside
  this phase and remains prohibited.

## Candidate mapping and validation

### Final 12-commit sequence

| # | New OID | Owner | Source OID(s) | Unit cases |
|---:|---|---|---|---:|
| 1 | `11b88fa72191c338b7720410602fc85434347bb0` | selective/name-only PubSub API | `65b19c9` | 7/7 |
| 2 | `ae50cb792f5cae60be96d38da26dae89a0885ff6` | piggyback Mapping plus late-data recovery | `38f5148`, `b99a66c` | 7/7 |
| 3 | `99a8ee911fba055cf8d8b870a5bd9319f30b8d6f` | parallel sync and batching | `d0e128d` | 7/7 |
| 4 | `f365727843a12258cad95144eaf36bdc49a46989` | async publication and parallel production | `0b56a94` | 7/7 |
| 5 | `f75e3ef282094181ac28da2e58e6c6aa1b6be77b` | InterestSigner migration | `627b273` | 8/8 |
| 6 | `a40df44e3ade3f56e23ff473284017d9cd602527` | typed piggyback bounds plus stable pending fetches | `7804202`, `e42996d` | 8/8 |
| 7 | `5910614a8ea414ca2d9e1cece82ec3098ba1afe5` | complete official V3 | `eb0754d` | 44/44 |
| 8 | `de35fe2049084476e73eb445f79fb8c0a266a40e` | async/piggyback regression coverage | `f8ef18f` | 47/47 |
| 9 | `de58f16ce5c8f9ea78912bf4ceb0a0a68233b57e` | Boost 1.71 fork baseline | `2a334c7` | 47/47 |
| 10 | `c148e4433a3da85f9bc8958e198c44077ea29ab5` | sparse Mapping without duplicate fetches | `cce63dc`, `e4687cb` | 49/49 |
| 11 | `c699642a068ed27d1562404323fde6444512ea5a` | transactional segmented publication recovery | `3ed1bc2`, `6939790` | 66/66 |
| 12 | `0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` | atomic Mapping/Repair extension transaction | `751ab20`, `8335643` | 71/71 |

Candidate count is 12 and candidate tree is
`fc5743665da77667930850969a1c05489733c8f4`, exactly equal to frozen source
tree. `git diff --check` passed for the whole range and for every surviving
commit.

### Exact-OID validation

Validation ran only after the complete candidate was first constructed.
Commits 1-8 used user-space Boost 1.74 from
`/tmp/boost-1.74-test-install`; commits 9-12 used the host Boost 1.71, directly
proving the fork baseline. Every checkout ran:

```text
git diff --check <oid>^..<oid>
./waf distclean
./waf configure --with-tests [Boost 1.74 paths for commits 1-8]
./waf build -j4
LD_LIBRARY_PATH=<checkout>/build[:Boost-1.74-lib] ./build/unit-tests --log_level=test_suite
```

Detailed per-stage logs are in
`/tmp/spec115-consolidated-commit-tests-20260716T001503Z/`.

The first unit invocation initially loaded the host's older installed
`libndn-svs.so`; after adding the exact checkout's `build/` directory first in
`LD_LIBRARY_PATH`, the unchanged OID passed 7/7. This was a validation-command
error, not a code failure.

The first version of owner 6 failed to compile because the folded iterator
fix named `PublicationKey`, which was introduced only by the following V3
commit. The owner was repaired in place to derive its snapshot key through
`decltype(m_fetchMap)::key_type`, descendants were regenerated, and validation
resumed from owner 6. No corrective descendant was added. The final owner 6
passed 8/8, all descendants passed, and final tree identity was restored
exactly.

## Exact final-head acceptance

After the 12-commit history was complete, the final identity
`0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` was rebuilt cleanly with
AddressSanitizer and passed 71/71 unit cases. The NDNSF-owned TypeScript/C++
standalone matrix passed 5/5, and immutable MiniNDN candidate
`spec114-0fbdec193b9cd3364071` passed all six formal 0%/5% cells exactly once
with 240/240 bidirectional observations, six equal final vectors, and zero
duplicates, rejects, restarts, or Sync-Ack packets. Exact commands, artifact
paths, and hashes are recorded in `evidence/full-validation.md`.

## CI-driven owner correction

The first published consolidated head revealed a missing direct `<atomic>`
include on six Ubuntu 22.04 compiler jobs. The correction was folded into
transaction owner 11, changing `c699642 -> b276ec1`; owner 12 replayed as
`0c09d65 -> 6b1fc52`. Owners 1-10, including official V3 owner `5910614`, are
unchanged. The corrected range still contains 12 commits and differs from the
first published tree by exactly one include line. Boost 1.71 unit and exact-head
network evidence are recorded in `evidence/full-validation.md`.
