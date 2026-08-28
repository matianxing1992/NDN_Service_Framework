# Nine-Owner Rewrite Validation Receipt

Captured: `2026-07-17`

## Identities

```text
base               a93724758aca71a4ea327574ef7af46770a81a40
frozen source      a965ad6c847ee86c90289ef3bab00a49ae042396
local master       ee4e174b0e5f188f4e48608c60898649c8021bc9
local Experimental ee4e174b0e5f188f4e48608c60898649c8021bc9
origin/master      0c09d65afc91bb25f2db1f0ce83cb20c0c6ebf8f
final/source tree  7b7acd4581caf20da81b6c628d7ca18497a75d9e
```

The rewrite ran in isolated branch
`spec115/nine-owner-20260717T020444Z-round3`. Safety refs retain the frozen
local master, Experimental, and origin identities. No push or PR edit occurred.

## Per-commit gate

Each exact OID was checked out as a newly created `compileTMP` branch. Temporary
commit `19ec38e` lowered only that branch's Boost threshold to 1.71. Validation
then ran `./waf distclean`, `./waf configure --with-tests`, parallel `./waf`,
and the complete unit executable with
`LD_LIBRARY_PATH=$PWD/build` so it could not load the stale installed library.
The source branch was restored and `compileTMP` deleted after every result.

| # | OID | Owner | Unit cases | Result |
|---:|---|---|---:|---|
| 1 | `815bd62` | API/name-only publication | 7 | PASS |
| 2 | `3b709b4` | bounded Mapping/Data piggyback | 10 | PASS |
| 3 | `cfc6270` | parallel receive/local batching | 10 | PASS |
| 4 | `e7cd98e` | parallel production/ordered async publish | 12 | PASS |
| 5 | `ee9632d` | V2 `InterestSigner` encoding | 13 | PASS |
| 6 | `9f679bd` | official V3 protocol | 48 | PASS |
| 7 | `c767560` | sparse Mapping recovery | 50 | PASS |
| 8 | `79d830a` | producer publication transaction | 59 | PASS |
| 9 | `ee4e174` | receiver fetch/Repair recovery | 71 | PASS |

Raw local logs are under
`/tmp/spec115-nine-validation-20260717T020444Z-final2/`. Every reviewed commit
retains Boost 1.74, `git show --check` passes for all nine OIDs, the final tree
equals the frozen source tree, and `git branch --list compileTMP` returns zero.

## Boundary corrections found by the gate

- Commit 2 received its own logger include/initialization rather than depending
  on commit 4.
- Commit 2 tests use the then-current V2 mapping signature; commit 6 migrates
  them to the BootstrapTime-aware V3 signature.
- Commit 4 signs the packet used by its async packet-publication test, matching
  the documented signed-packet precondition.
- Unit execution explicitly selects the worktree library; an earlier diagnostic
  run linked `/usr/local/lib/libndn-svs.so` and was rejected as inadmissible.

## Semantic audit

`79d830a` owns producer reservation, signed packet fitting, staging, ordered
commit, visibility, and rollback. `ee4e174` separately owns receiver fetch
windows, deadlines, adaptive lifetimes, retry/backoff, late-callback fencing,
segment validation, Repair, and atomic Mapping/Repair extension processing.
The split satisfies FR-028 without changing runtime content.
