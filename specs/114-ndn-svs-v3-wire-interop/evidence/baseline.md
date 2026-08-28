# Spec 114 Baseline Evidence

**Captured**: 2026-07-16  
**Status**: measured before Spec 114 protocol edits

## Target identity and rollback

- repository: `/home/tianxing/NDN/ndn-svs`
- branch: `Experimental`
- HEAD: `c34c04d766836bba1567a70bae846dfbd9d25b66`
- origin/master: `db9fc25a5d5c27a44506da343823ef92d085a9c2`
- relationship: Experimental is four commits ahead and zero behind origin/master
- safety ref: `refs/heads/safety/spec114-pre-v3` -> `c34c04d766836bba1567a70bae846dfbd9d25b66`
- target worktree before edits: clean
- remote mutation: none

Preserved Spec 113 commits, oldest first:

1. `692af112117f3eb5e21c251d5cc23c8a6f2089ad` — sparse mappings and duplicate-fetch suppression
2. `73bac35a1a6204858bedaf5c41ff716278704f31` — bounded segmented recovery
3. `5c8141aed5bcd489c7bcaedd521acc5b9be13438` — transactional asynchronous publication
4. `c34c04d766836bba1567a70bae846dfbd9d25b66` — Boost 1.71 fork baseline

Rollback is non-destructive:

```bash
git -C /home/tianxing/NDN/ndn-svs worktree add /tmp/spec114-pre-v3 safety/spec114-pre-v3
```

## Toolchain and installed identity

- compiler: `g++ 9.4.0` on Ubuntu 20.04
- ndn-cxx: `0.9.0`
- Boost: `1.71.0`
- installed `/usr/local/include/ndn-svs/core.hpp` SHA-256:
  `9f35096337e281e7bd34ddebf814824e33a015fdb16b9997c3d9d25ce2e7d2bf`
- installed `/usr/local/lib/libndn-svs.so.0.1.0` SHA-256:
  `a64436b80c0fb7c8ce49e947b02b6e83fe7a0661d39b9a45a415d74e1843ba43`

## Measured hybrid wire packet

A temporary public-API probe constructed `SVSyncCore` over
`DummyClientFace`, waited for prefix registration, invoked initial Sync
production in serial and parallel modes, and inspected the emitted Interest.

| Field | Serial | Parallel |
|---|---|---|
| name | `/ndn/spec114/baseline/v=2/<parameters-digest>` | same |
| InterestLifetime | 1 ms | 1 ms |
| ApplicationParameters first/only child | TLV 201 `StateVector` | TLV 201 `StateVector` |
| embedded signed Data | absent | absent |
| full Interest SHA-256 | `9eeba46f2eb48dee0ced50e76e7db4d4ceb67adf3361d8a14598525f7216b84d` | `e73809cddd3d3e3d621f49c40eeae85865e8ee794b773aefd8a93d74e8c4ab34` |

The two wire hashes differ only in nondeterministic Interest fields/signature;
their version, lifetime, and raw-StateVector envelope match. This confirms the
reported incompatibility: current Experimental uses the V2 route and V2 raw
ApplicationParameters while its StateVector entries already carry V3 bootstrap
tuples.

## Pinned authorities

- normative SVS V3 page: last update `2025-01-14`,
  `https://named-data.github.io/StateVectorSync/Specification.html`
- SVS-PS page: last update `2025-01-04`,
  `https://named-data.github.io/StateVectorSync/PubSubSpec.html`
- NDNts repository main identity observed for pinning:
  `63c489f23347a7cc64c0f62897901e291b8f3cc4`
- NDNts package: `@ndn/svs@0.0.20250307`; its published declarations expose
  the explicit `svs3` option and its dependency engine accepts Node 22
- npm integrity:
  `sha512-ghMT6ekrMrYryZztduf7pDkAm20MpXamr3lMFG6K7aNCRY16Uyx6cvL60N18jZjqwpKfjGDazhjVvTJmHcCoGg==`
- generated `package-lock.json` SHA-256:
  `ef41e08e787c74c9fc424c96684f8c163a017ffd1a3b82bc064bf75df7ffd95c`
- lockfile audit: 119 packages, zero known vulnerabilities; the initially
  considered 20260427 package was rejected because it requires Node 24/25
- upstream C++ master identity observed for comparison:
  `a93724758aca71a4ea327574ef7af46770a81a40`
- the NDNts interop README explicitly uses C++ for SVS V2 and NDNd for SVS V3;
  it does not constitute proof that current C++ Experimental is V3 compatible.
