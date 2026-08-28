# Final Commit Review

**Date**: 2026-07-15  
**Baseline**: `db9fc25a5d5c27a44506da343823ef92d085a9c2`  
**Validated HEAD**: `c34c04d766836bba1567a70bae846dfbd9d25b66`

| OID | Concern | Tests in/covering commit | Independent revert effect |
|---|---|---|---|
| `692af112117f3eb5e21c251d5cc23c8a6f2089ad` | sparse mappings and duplicate-fetch suppression | MappingProvider range plus PubSub in-flight/backoff cases | restores all-or-nothing mapping replies and permits repeated fetch scheduling |
| `73bac35a1a6204858bedaf5c41ff716278704f31` | bounded segmented publication recovery | adaptive lifetime, repair, segmentation, assembly, storage failure, shutdown cases | removes the bounded large-publication recovery path while leaving mapping commit intact |
| `5c8141aed5bcd489c7bcaedd521acc5b9be13438` | transactional async publication and callback lifetime | ordered retry, Face fallback, rollback capability, actual boundary, Core send containment, Fetcher terminal callbacks | restores the pre-hardening segmented recovery implementation |
| `c34c04d766836bba1567a70bae846dfbd9d25b66` | Boost 1.71 fork build baseline | clean configure/build | restores upstream 1.74 configure requirement without changing runtime |

Commit 2 intentionally owns the cohesive **bounded segmented publication
recovery** foundation: segmentation/storage is required to make recovery of the
reported 6.5–16-KB publications possible. Commit 3 independently owns the
transaction/ordering/lifetime invariants found by the merge audit. Thus a
reviewer can reject the hardening delta, the full segmented-recovery feature,
mapping behavior, or the fork build policy independently.

For each commit, `git diff --check <oid>^ <oid>` passed. Subjects contain no WIP
or experimental wording. The final range contains no `.gitignore`, agent,
Spec Kit, docs, results, NDNSF-DI, UAV, Repo, container, or custom
security-options delta. The reviewed `InterestSigner` remains unchanged.

The final committed product tree exactly matches
`backup/spec113-tested-final-20260715` after excluding its local recovery-only
artifacts. The active review worktree is clean.

## Committed-Source Rebuild

Executed from clean `c34c04d`:

```bash
./waf clean
./waf configure --with-tests
./waf -j4
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests --log_level=test_suite
```

Boost 1.71 configuration and all 17 build steps passed. `ldd` bound the test
binary to the rebuilt worktree library. Result: 42/42 test cases passed.

| Artifact | SHA-256 |
|---|---|
| `build/libndn-svs.so` | `a64436b80c0fb7c8ce49e947b02b6e83fe7a0661d39b9a45a415d74e1843ba43` |
| `build/unit-tests` | `7092fff7096a7c6281fda0bb39563c36a6de24f41cce6ff980064fe88f200f7c` |

