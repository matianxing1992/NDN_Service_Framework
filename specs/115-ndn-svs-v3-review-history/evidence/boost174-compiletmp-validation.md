# NDN-SVS Boost 1.74 Review and `compileTMP` Validation

Captured: `2026-07-17`

## Scope and invariant

This rule applies only to `/home/tianxing/NDN/ndn-svs`. NDNSF and other local
repositories retain their existing Boost 1.71 policy.

Every reviewed NDN-SVS commit keeps the tracked Boost 1.74 requirement:

```text
BOOST_VERSION_NUMBER < 107400
The minimum supported version of Boost is 1.74.0.
```

The former `build: retain the Boost 1.71 fork baseline` commit is absent from
the final review range. Local compilation creates `compileTMP` at each exact
reviewed OID, temporarily applies the one-file 1.71 threshold patch, configures,
builds, and runs the complete available unit suite, then switches back and
deletes `compileTMP`.

## Final identities

| Identity | OID |
|---|---|
| local `master` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` |
| local `Experimental` | `ee4e174b0e5f188f4e48608c60898649c8021bc9` |
| final tree | `7b7acd4581caf20da81b6c628d7ca18497a75d9e` |
| origin/PR head, unchanged | `0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f` |
| upstream master, unchanged | `a93724758aca71a4ea327574ef7af46770a81a40` |

The final tree equals the frozen source candidate
`a965ad6c847ee86c90289ef3bab00a49ae042396` exactly.

## Exact-OID unit results

| # | Exact OID | Tree | Unit result |
|---:|---|---|---:|
| 1 | `815bd627fc14a372948ed4118b713542d0b81a5d` | `f38cd56cca6d1843d22369145fc7739f214c3e5d` | 7/7 PASS |
| 2 | `3b709b4a1c612fd109814608caa9154a10639065` | `945a321d473f44f29e8349a83ce60373f3e37420` | 10/10 PASS |
| 3 | `cfc6270547a9564f41f32f8799d3e6496963ed11` | `2fc414bc83ba1cd069a50556917a6568fdfb3745` | 10/10 PASS |
| 4 | `e7cd98ea48ff03d322dbe9db2f9d00ea7608c35c` | `b03d91f7be26bbaf7dc7152f40785394bd6774c1` | 12/12 PASS |
| 5 | `ee9632d76272c6b2bf7a748bcfd1a47533596e38` | `0d160d4c8286f8d8750daaefd6d96e155161efd6` | 13/13 PASS |
| 6 | `9f679bdb1bbaaa0c351c3179fa25beaef881ef84` | `3aa112e960570526bf65bf60b8bf86b889ec4af2` | 48/48 PASS |
| 7 | `c7675606cc062a9dbd774e2705094ca98019e566` | `f1068d1cc36557b74763be324746e39724510be1` | 50/50 PASS |
| 8 | `79d830a368a7e2cf73f4b8d6db295c1d404554dc` | `d10cb1e07b09c6618b0402e0dff82faf996f74fb` | 59/59 PASS |
| 9 | `ee4e174b0e5f188f4e48608c60898649c8021bc9` | `7b7acd4581caf20da81b6c628d7ca18497a75d9e` | 71/71 PASS |

Logs:

```text
/tmp/spec115-nine-validation-20260717T020444Z-final2/
```

Every configure log reports Boost `1.71.0`; every unit log reports no errors.
After each result, validation returned to the candidate branch and deleted
`compileTMP`. The final branch scan reports `compileTMP=0`.

## Ownership correction

`setPeriodicSyncTime()` is introduced by commit 3,
`cfc6270547a9564f41f32f8799d3e6496963ed11`, together with parallel Sync
processing and local Sync Interest batching. Piggyback bounds are folded into
`3b709b4a1c612fd109814608caa9154a10639065`. Producer publication transactions
and receiver fetch/Repair recovery are separate owners `79d830a` and `ee4e174`.

## Final gates

- surviving commits: 9;
- per-commit tracked Boost threshold: 9/9 at 1.74;
- tracked Boost 1.71 policy markers: 0;
- per-commit `git diff --check`: 9/9 PASS;
- per-commit `compileTMP` unit suites: 9/9 PASS;
- final `compileTMP` branch count: 0;
- local `master == Experimental`: PASS;
- remote/upstream mutation: none;
- push performed: no.
