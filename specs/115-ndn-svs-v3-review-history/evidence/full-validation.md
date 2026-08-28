# Final Candidate Validation

> Historical evidence for the earlier 12-commit Boost 1.71 candidate. The
> controlling final identities and local build procedure are in
> `boost174-compiletmp-validation.md`; results below remain evidence for the
> unchanged runtime tree, not for final commit IDs or dependency policy.

## Active identities and ownership

```text
official V3:        eb0754d9d250f65789928cfdf7f3917fae996f07
local master:       8335643f81be8fe3d49cf6e773569a21762049c9
local Experimental: 8335643f81be8fe3d49cf6e773569a21762049c9
Experimental tree:  fc5743665da77667930850969a1c05489733c8f4
origin/master:      2b052c94444044cb34eb4160b6e76d6564c7c918 (unchanged)
```

`eb0754d` is an ancestor of both active local branches. NDN-SVS master and
Experimental track no `tests/interop`, Node package lock, Node dependencies,
or external peer program. The NDN-SVS worktree is clean.

The executable interoperability example is owned by NDNSF:

```text
examples/interop/ndn-svs-v3/cpp/svs3-peer.cpp
examples/interop/ndn-svs-v3/ndnts/svs3-peer.ts
examples/interop/ndn-svs-v3/ndnts/package.json
examples/interop/ndn-svs-v3/ndnts/package-lock.json
examples/interop/ndn-svs-v3/build-cpp-peer.sh
examples/interop/ndn-svs-v3/run-standalone.sh
Experiments/NDN_SVS_V3_Interop_Minindn.py
```

The standalone and MiniNDN results below were executed against pre-rewrite
`70e682f`. Phase 10 proved `8335643f` has the identical
`fc5743665da77667930850969a1c05489733c8f4` tree and reran every exact-OID unit
gate, but did not rerun the network matrix merely because commit ownership and
messages changed. Network evidence therefore remains historical/tree-bound,
not falsely represented as exact-new-OID execution.

The NDNts peer is actual TypeScript, not renamed JavaScript. Node 22 parsed and
executed its type annotations directly. Its events identify the implementation
as `ndnts-typescript`.

## Source and unit gates

| Gate | Result |
|---|---|
| clean NDN-SVS Experimental configure/build with AddressSanitizer | PASS |
| complete NDN-SVS unit suite | PASS, 71/71 |
| NDN-SVS active-ref external-harness scan | PASS, zero files |
| NDNSF candidate/ownership tests | PASS, 3/3 + 11/11 |
| Node 22 TypeScript syntax/runtime loading | PASS |
| pinned NDNts installation | PASS, 118 packages, 0 vulnerabilities |
| NDNSF external C++ peer build | PASS |

## NDNSF-owned standalone interoperability

Output: `/tmp/spec115-ndnsf-owned-standalone-70e682f-v1`

| Case | Result |
|---|---|
| C++ to NDNts TypeScript V3 | PASS, 5/5 observed |
| NDNts TypeScript to C++ V3 | PASS, 5/5 observed |
| concurrent bidirectional V3 | PASS, 5/5 each direction |
| explicit V2 | PASS, 5/5 each direction |
| V2/V3 mismatch isolation | PASS, 0/0 as expected |

The summary explicitly records `ndntsSourceLanguage=TypeScript` and
`implementation=ndnts-typescript`. No manual route was injected. Summary
SHA-256:

```text
129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b
```

## NDNSF-owned formal MiniNDN matrix

Frozen exact-candidate directory:

```text
results/spec115-svs-v3-ndnsf-owned-final/spec114-88ab43d64939fa36eb63/
```

The v2 candidate manifest binds clean `Experimental@70e682f`, its tree, the
NDNSF C++ and TypeScript sources, build/runner scripts, Node v22.23.1, and the
pinned NDNts lock.

| Cell | Loss | C++ sees TypeScript | TypeScript sees C++ | Final vectors | Negatives | Result |
|---|---:|---:|---:|---|---:|---|
| loss00-run01 | 0% | 20/20 | 20/20 | equal | 0 | PASS |
| loss00-run02 | 0% | 20/20 | 20/20 | equal | 0 | PASS |
| loss00-run03 | 0% | 20/20 | 20/20 | equal | 0 | PASS |
| loss05-run01 | 5% | 20/20 | 20/20 | equal | 0 | PASS |
| loss05-run02 | 5% | 20/20 | 20/20 | equal | 0 | PASS |
| loss05-run03 | 5% | 20/20 | 20/20 | equal | 0 | PASS |

All six cells record `interopPeers.owner=NDNSF` and
`interopPeers.ndnts.language=TypeScript`. Total bidirectional remote coverage
is 240/240; final vectors are equal in 6/6 cells; duplicate coverage, rejects,
peer restarts, and Sync-Ack packets total zero. Cells ran exactly once and took
460.410 seconds in total.

```text
TypeScript source SHA-256
d76bcac680104637dfe352d61221712b30d126a4b2666d3e295c9468507cdb16

executed C++ peer SHA-256
0b354f9c8cbcfc3d239d8c7017e02b91c1d7a2bc8bf266c041c617f7bd396fd0

candidate-manifest.json SHA-256
26eff28d2ea2446fdf22792d143f5a374931d17e3a16151421e0aa4ec76dc34f

formal-summary.json SHA-256
540986807e6d5f35e2ccdd531266021390ecfd9f14ec30d1e716e5b7f5f9388f
```

Root-created artifact ownership was corrected after execution. Aggregate-only
verification regenerated the same successful summary without rerunning cells.

## Evidence boundary

This proves actual NDNSF-owned TypeScript NDNts and C++ NDN-SVS consumers work
together through NFD inside MiniNDN. It does not claim final-OID GitHub Actions
or publication because origin and PR #36 remain unchanged.

## Exact consolidated-head rerun

Phase 12 removed the remaining tree-only qualification by rebuilding and
executing the complete network gate against the final consolidated identity:

```text
NDN-SVS Experimental: 0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
NDN-SVS tree:         fc5743665da77667930850969a1c05489733c8f4
official V3 owner:    5910614a8ea414ca2d9e1cece82ec3098ba1afe5
```

The clean exact-head build used AddressSanitizer and passed the complete unit
suite, 71/71. The preserved log is
`/tmp/spec115-final-asan-units-0c09d65.log` with SHA-256
`f0ce1944bd48fbc44a3e3a668adce3382a132738290904e16f8452e2b250c5ed`.

The NDNSF-owned peer assets were then rebuilt against that exact checkout. The
executed C++ peer SHA-256 is
`0400b8180201be0d64c65578772795676244a35e31bd1f30f8f933209d537fc0`;
the TypeScript source SHA-256 is
`59a7fba6c63f4dd9a1f1c7cfed01cbb907d87a1c183a0f96f1cee929ae4434ea`;
and the pinned lock SHA-256 is
`4a18d616980fc4aa0bb0e251f6b241cd04fb8e5565c3a102066e4d9d47f45209`.

The five-case standalone matrix at
`/tmp/spec115-final-standalone-0c09d65-20260716T003332Z` passed 5/5: both
unidirectional V3 directions, concurrent bidirectional V3, explicit V2, and
V2/V3 mismatch isolation. It observed no Sync-Ack packets and its summary
SHA-256 is
`129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b`.

The new immutable MiniNDN candidate is:

```text
results/spec115-svs-v3-final-0c09d65/spec114-0fbdec193b9cd3364071/
```

All six formal cells ran exactly once: three at 0% loss and three at 5% loss,
20 publications per peer, with the fixed 60-second per-cell convergence bound.
Every cell passed with 20/20 observations in each direction, equal final
vectors, and zero duplicate coverage, rejects, peer restarts, and Sync-Ack
packets. Aggregate bidirectional coverage was 240/240 and elapsed matrix time
was 493.759 seconds. Aggregate-only verification passed without rerunning any
network cell.

```text
candidate-manifest.json SHA-256
00433c99d32f68a836ec3a4d54c240c94b3bd03f0df1a84bd0c076c8ac648bcf

formal-summary.json SHA-256
985d73f91140d775a64b5d77c94ad0accf15d646cf1b7208214d25feb132cca6
```

This evidence is bound to the final consolidated OID, not merely to an equal
tree. Remote publication and exact-head GitHub Actions remain separate T033
and T034 gates.

## GitHub old-compiler correction and local revalidation

GitHub run `29545641123` on published head `0c09d65` completed with 25 passing
jobs and six failing Ubuntu 22.04 jobs: g++ 10/11 and clang 11-14. The common
diagnostic was:

```text
ndn-svs/fetcher.hpp: no template named 'atomic_bool' in namespace 'std'
```

`fetcher.hpp` used `std::atomic_bool` but did not directly include `<atomic>`.
`git log -S` placed the introduction in owner 11. The include was folded there,
creating owner `b276ec1` and replayed head `6b1fc52`; the first ten commits and
official V3 owner `5910614` are unchanged. The complete tree difference from
published `0c09d65` is one include line.

Per the user's revised validation rule, only the fork baseline Boost 1.71 was
built locally:

| Exact OID | Toolchain | Result |
|---|---|---|
| `b276ec1` | g++ 9.4, Boost 1.71 | PASS, 66/66 |
| `6b1fc52` | clang 10, Boost 1.71, ASan | PASS, 71/71 |

The corrected standalone output is
`/tmp/spec115-ci-fix-standalone-6b1fc52-20260717T0055Z-v2/`; all five cases
passed and summary SHA-256 is
`129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b`.

The corrected immutable MiniNDN candidate is:

```text
results/spec115-svs-v3-ci-fix-6b1fc52/spec114-a049bf032f73fc62e192/
```

Its six cells ran exactly once and all passed: 240/240 bidirectional coverage,
six equal final vectors, and zero duplicates, rejects, peer restarts, or
Sync-Ack packets. Aggregate-only verification passed without rerunning cells.

```text
candidate-manifest.json SHA-256
0127587c05237fd91ae9c7cc601378e2ea085b00cff57096a7fd97d9dd109d52

formal-summary.json SHA-256
d273de80527c355b6aa6f06bca18df18fa1b089bbdb6209d9daafb2420e71b02

executed C++ peer SHA-256
7b497ec248d54856ed1db36c2b1133b2502f68d0aecc8d07ec554df54062500b
```

This resolves the code and local evidence failure. It does not resolve the
remote gate: `origin/master` and PR #36 remain on failing head `0c09d65` because
the user prohibited further pushes until a new explicit request.
