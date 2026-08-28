# Spec 115 Completion Summary

## 2026-08-07 Focused-PR Outcome

The focused branch is published; the reorganized local master remains local:

```text
master             3c96ab4 (complete reorganized 9-commit history)
pr/svs-v3-focused  5db5e6a (first 3 focused commits)
```

The focused branch is the pull-request candidate. It contains regex/name-only
PubSub, V2 InterestSigner, and corrected SVS V3 in that order. All Mapping,
parallel, segmented-publication, and Fetcher/Repair work follows the focused
boundary only on local `master`. Exact focused units pass 40/40, exact master
units pass 74/74, and focused standalone C++/TypeScript interoperability passes 5/5.
No code-unrelated path was added to NDN-SVS, the original dirty Experimental
checkout is unchanged. Only `origin/pr/svs-v3-focused@5db5e6a` was created;
`origin/master`, tags, and PR metadata were not modified. See
`evidence/focused-v3-rewrite-20260807.md` for immutable identities and hashes.

The sections below document the previous nine-owner consolidation and remain
historical. The focused-PR override above is the current controlling outcome.

## Current outcome

The local NDN-SVS range is now consolidated into nine behavior-owned commits.
Every reviewed commit permanently requires Boost 1.74. The former committed
Boost 1.71 build-policy owner was removed; host Boost 1.71 is used only on a
disposable `compileTMP` branch during local compilation.

`setPeriodicSyncTime()` belongs to the parallel-Sync owner. Piggyback bounds
and their tests are folded into one owner, and async tests are folded into the
ordered-production owner. Producer-side segmented publication and receiver-side
fetch/Repair recovery are separate commits because they implement different
state machines. Every exact OID is locally validated. Standalone/MiniNDN
results remain tree-equivalent historical evidence; publication still requires
a new explicit request.

## Current identities

| Identity | OID / state |
|---|---|
| official V3 owner | `9f679bdb1bbaaa0c351c3179fa25beaef881ef84` |
| local `master` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` |
| local `Experimental` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` |
| final local tree | `7b7acd4581caf20da81b6c628d7ca18497a75d9e` |
| `origin/master` and PR #36 head | `0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` |
| remote pre-publication safety branch | `safety/spec115-pre-final-publication-20260717T004458Z -> 2b052c94444044cb34eb4160b6e76d6564c7c918` |
| local published-head safety ref | `safety/spec115-ci-old-head-20260717T004955Z -> 0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` |

The final tree exactly equals the frozen source candidate `a965ad6`; this was a
history/ownership correction, not a runtime redesign.

## Corrected review sequence

```text
815bd62  svspubsub: add regex subscriptions and name-only publishing
3b709b4  svspubsub: piggyback bounded mappings and publication Data
cfc6270  sync: parallelize receive processing and batch local Sync Interests
e7cd98e  sync: parallelize production and add ordered async PubSub publishing
ee9632d  security: encode V2 signed Sync Interests with InterestSigner
9f679bd  svs: implement the version 3 synchronization protocol
c767560  svspubsub: recover sparse mappings without duplicate fetches
79d830a  svspubsub: make segmented publication failure-atomic
ee4e174  svspubsub: bound segmented fetch and repair recovery
```

## Local validation

The reviewed history uses Boost 1.74 only. Each local compile created
`compileTMP` at the exact reviewed OID, temporarily applied the one-file Boost
1.71 threshold patch, ran the complete unit suite, returned to the candidate,
and deleted `compileTMP`. This rule applies only to NDN-SVS; NDNSF remains on
its existing Boost 1.71 policy.

| Gate | Result |
|---|---|
| tracked Boost requirement | 9/9 commits at 1.74 |
| disposable Boost 1.71 unit builds | 9/9 commits PASS, 7-71 cases |
| whole-range and per-commit `git diff --check` | PASS |
| final `compileTMP` branch count | 0 |
| standalone C++/NDNts TypeScript matrix | PASS, 5/5 |
| immutable MiniNDN formal matrix | PASS, 6/6 |
| MiniNDN bidirectional coverage | 240/240 |
| MiniNDN final vectors | equal, 6/6 |
| duplicates / rejects / restarts / Sync-Ack | 0 / 0 / 0 / 0 |

Evidence:

```text
/tmp/spec115-nine-validation-20260717T020444Z-final2/
specs/115-ndn-svs-v3-review-history/evidence/boost174-compiletmp-validation.md
specs/115-ndn-svs-v3-review-history/evidence/nine-owner-validation.md

/tmp/spec115-ci-fix-standalone-6b1fc52-20260717T0055Z-v2/
  summary SHA-256 129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b

results/spec115-svs-v3-ci-fix-6b1fc52/spec114-a049bf032f73fc62e192/
  candidate-manifest.json SHA-256 0127587c05237fd91ae9c7cc601378e2ea085b00cff57096a7fd97d9dd109d52
  formal-summary.json SHA-256 d273de80527c355b6aa6f06bca18df18fa1b089bbdb6209d9daafb2420e71b02
```

## Remote state and remaining work

The old published head completed 25/31 GitHub jobs and failed the six Ubuntu
22.04 old-compiler jobs because of the missing `<atomic>` include. Those failed
checks are not relabeled as green. The corrected local candidate has not been
pushed, so it has no exact-head GitHub Actions result.

No further push is authorized. T033 and T034 therefore remain open until the
user explicitly requests publication. At that point the next agent must:

1. refresh and verify remote master still equals `0c09d65`;
2. create another unique remote safety ref for that exact head;
3. publish `ee4e174` with an exact `--force-with-lease`;
4. update the PR description from `0c09d65` to `ee4e174`;
5. require all exact-head GitHub Actions jobs to pass;
6. perform the final remote and Spec Kit audit.

This summary does not claim Spec 115 complete, upstream acceptance, merge, or
release.
