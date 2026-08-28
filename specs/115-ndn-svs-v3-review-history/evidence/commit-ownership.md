# Spec 115 Commit Ownership

## Current controlling identities

```text
official V3 owner:  9f679bdb1bbaaa0c351c3179fa25beaef881ef84
local master:       ee4e174b0e5f188f4e48608c60898649c8021bc9
local Experimental: ee4e174b0e5f188f4e48608c60898649c8021bc9
origin/PR head:      0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
```

`9f679bd` owns the normative `<group-prefix>/v=3` envelope, embedded signed
StateVector Data, ParametersSha256DigestComponent, BootstrapTime-aware state,
validation before mutation, explicit V2 isolation, group-prefix NFD
registration with version-specific local dispatch, deterministic fixtures,
and directly coupled tests. It is unchanged by the old-compiler correction.

## Final nine owners

| # | Commit | Owned behavior |
|---:|---|---|
| 1 | `815bd62` | regex subscriptions and name-only publishing |
| 2 | `3b709b4` | bounded Mapping/Data piggyback delivery, cache lifecycle, and tests |
| 3 | `cfc6270` | bounded parallel Sync receive, local batching, and periodic timer override |
| 4 | `e7cd98e` | parallel production, ordered async publication, and async tests |
| 5 | `ee9632d` | V2-only `InterestSigner` encoding |
| 6 | `9f679bd` | complete official SVS V3 protocol, V2 profile propagation, and tests |
| 7 | `c767560` | sparse Mapping recovery without duplicate fetches |
| 8 | `79d830a` | producer reservation, signed-packet fitting, staging, ordered commit, visibility, and rollback |
| 9 | `ee4e174` | receiver fetch window, deadline, retry/backoff, validation, Repair, and atomic Mapping/Repair extensions |

The old-compiler repair remains folded into the transaction owner where
`std::atomic_bool` first appears. The former Boost 1.71 build-policy commit is
absent; every tracked owner retains Boost 1.74. Local builds use disposable
`compileTMP` branches documented in `boost174-compiletmp-validation.md`.

Mapping/Repair policy remains outside the official V3 owner. The executable
C++/NDNts harness is owned by NDNSF under
`examples/interop/ndn-svs-v3/`; no external Node dependency or peer program is
tracked by NDN-SVS master or Experimental.

## Machine-checkable manifest and validation

- manifest: `evidence/commit-ownership.json`;
- verifier: `Experiments/spec115_v3_history_rewrite.py`;
- verifier tests: `tests/python/test_spec115_v3_history_rewrite.py` (4/4);
- official V3 owner count: exactly one;
- surviving review commits: 9;
- tracked Boost 1.74 policy: 9/9 commits;
- disposable Boost 1.71 `compileTMP` unit gates: 9/9 PASS;
- standalone interoperability: 5/5;
- formal MiniNDN matrix: 6/6.

## Recovery and publication boundary

- `safety/spec115-ci-old-head-20260717T004955Z` retains published `0c09d65`;
- remote `safety/spec115-pre-final-publication-20260717T004458Z` retains
  pre-publication `2b052c9`;
- corrected candidate `ee4e174` remains local by explicit user instruction;
- no statement in this file claims green exact-head remote CI, upstream merge,
  or release.
