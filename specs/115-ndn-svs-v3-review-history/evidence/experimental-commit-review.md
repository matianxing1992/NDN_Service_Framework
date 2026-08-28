# Experimental Commit Reviewability Rewrite

> Historical review stage. Its committed Boost 1.71 owner was removed from the
> final sequence; current policy and evidence are in
> `boost174-compiletmp-validation.md`.

## Frozen authority

- local `master`: `e42996df0bd5b2d3fbe032cbb31ae83c04a46b1e`
- local `Experimental`: `70e682f8500e2ad205ec17722287ea0b8bd6a9f0`
- `origin/master`: `2b052c94444044cb34eb4160b6e76d6564c7c918`
- accepted Experimental tree: `fc5743665da77667930850969a1c05489733c8f4`
- immutable local rollback ref:
  `safety/spec115-experimental-review-20260716T230617Z`
- remote mutation: prohibited for this phase

## Pre-rewrite audit

| Old commit | Finding | New ownership |
|---|---|---|
| `9e65ad5` | Sparse Mapping responses and duplicate-fetch suppression answer different reviewer questions and have independent rollback value. | Split into sparse Mapping and duplicate suppression commits. |
| `443ae9b` | Adaptive fetch, assembly, signed outer-packet sizing, repair, timeout state, sequence visibility, and store rollback share one recovery state machine. | Keep atomic; a split would create a partial or unbuildable intermediate revision. |
| `cd7bb54` | Reservation, staging, commit, abort, and callback completion are one transactional state machine. | Keep atomic. |
| `357c44b` | Build baseline appears after source changes, so preceding commits are not independently testable on the supported Boost 1.71 environment. | Move first. |
| `70e682f` | Generic validated extension transport is mixed with fork-specific Mapping/Repair policy. | Split core transport from SVSPubSub policy. |

## Candidate sequence

1. `build: retain the Boost 1.71 fork baseline`
2. `svspubsub: support sparse Mapping responses`
3. `svspubsub: suppress duplicate Mapping fetches`
4. `svspubsub: add bounded segmented publication recovery`
5. `svspubsub: make asynchronous publication transactional`
6. `sync: validate and bound extension transport`
7. `svspubsub: add bounded Mapping and Repair extension policy`

## Acceptance gate

Each exact candidate OID must configure, build, and pass the complete unit
suite before the next commit is accepted. The final tree must equal
`fc5743665da77667930850969a1c05489733c8f4`; otherwise the live local branch is
not moved. `master`, `origin/master`, tags, remotes, and PR #36 remain untouched.

## Results

### Final review sequence and per-commit unit gate

| Exact OID | Tree | Complete unit result | Unit-log SHA-256 |
|---|---|---:|---|
| `2a334c7acc607d9259dce22361f10dd307794397` | `d9c239e2e768571a0206d24b8ce78b1c73fe6925` | PASS, 47/47 | `ab00d9393c5a6698c43ba18c55354f04e7fc04f7050c780631e421fa9e88c13d` |
| `cce63dca434277a839f0a3e4648dde571c7bf6e9` | `aa932c399cd8e0f795dfced15ad22124f2e7c3de` | PASS, 48/48 | `aeb3e0fe132eec986b827d935c102c60631460773f697fa59c2f5f0c20dde609` |
| `e4687cb89a8a618880d002dc534a820467b5cba3` | `4897b317ea78041548b0ba7e1fbd71f9f8ad6a5b` | PASS, 49/49 | `804b7779244e053a24ecbed51ecf1c7c44b873e54593efe7b2213f3ce50978b1` |
| `3ed1bc22473e30a3ce62a08fbd22ea7f1be2d9ae` | `7dde2152b400626f7473f4b3bbf82832bd131ba0` | PASS, 60/60 | `71ff7b76ed764a1e704ae597ea6620d679e9bbce461776090e297a4cbecb823f` |
| `6939790dd1b012a5125982b7e256be73eaf47d39` | `c46d673c1c6121b3d32f7c1d4dace0e527a5bf2d` | PASS, 66/66 | `bc9dfeb03286c275597764dd26b473fc79f36fd006503f3934da799d02f58600` |
| `751ab209a1453be7745b6d5e659246961768fc4a` | `e73b54e0b0d9c7dcb195b302118bd2ebba7d3575` | PASS, 67/67 | `80b44e728313537cbf2a4568328699bf124b48c2eb6b909f298bb50245f5013e` |
| `8335643f81be8fe3d49cf6e773569a21762049c9` | `fc5743665da77667930850969a1c05489733c8f4` | PASS, 71/71 | `ab6644796b8a16c9fa54f89782455f1d0b3f1c50cfd97b2c415f059c5fb99cfd` |

Every row used:

```bash
./waf distclean
./waf configure --with-tests
./waf -j"$(nproc)" --targets=unit-tests
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests --log_level=test_suite
```

The final exact head also passed AddressSanitizer, 71/71, using
`--with-sanitizer=address` and `ASAN_OPTIONS=detect_leaks=1`. Its log digest is
`2be10f66fc4fa46db8816bf8c5b3804cc464b7ac4d4e59a42881e6b7b7c8352a`.
Logs remain in
`/tmp/spec115-experimental-review-results-20260716T230617Z/`.

An initial test launch omitted the local build directory from
`LD_LIBRARY_PATH` and loaded an incompatible installed `libndn-svs`; its ABI
errors and `/v=3` registration mismatch are rejected evidence. The exact same
`820d974` tree passed 47/47 after the loader path was corrected. The first
sparse-Mapping candidate then exposed a real missing direct test include; that
include was moved into its owning commit before the final seven exact OIDs were
validated.

### Preservation and branch result

- old `9e65ad5` became `cce63dca` + `e4687cb8`;
- old `443ae9b` became atomic `3ed1bc22`;
- old `cd7bb54` became `6939790d`;
- old `357c44b` became the first commit, `2a334c7a`;
- old `70e682f` became `751ab209` + `8335643f`;
- final tree: unchanged at `fc5743665da77667930850969a1c05489733c8f4`;
- local `Experimental`: moved to `8335643f81be8fe3d49cf6e773569a21762049c9`;
- local `master`: unchanged at `e42996df0bd5b2d3fbe032cbb31ae83c04a46b1e`;
- `origin/master`: unchanged at `2b052c94444044cb34eb4160b6e76d6564c7c918`;
- tags, remotes, upstream refs, and PR #36: unchanged.

Rollback remains available through:

```bash
git switch master
git branch -f Experimental safety/spec115-experimental-review-20260716T230617Z
git switch Experimental
```
