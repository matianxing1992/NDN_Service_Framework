# Spec 117 Completion Summary

**Status**: implementation and bounded diagnosis complete; application-payload
interoperability is **not established**.

## Achieved claim

- `implemented`: an NDNSF-owned C++ `SVSPubSub` peer, real TypeScript NDNts
  `SvPublisher`/`SvSubscriber` peer, deterministic four-case corpus, exact
  receipt oracle, standalone runner, and conditional MiniNDN launcher exist.
- `executed`: both real peers published text, opaque binary, 4 KiB, and 32 KiB
  segmented payloads through their public APIs during one bounded standalone
  run.
- `measured-negative`: V3 StateVector updates crossed in both directions, but
  zero of eight expected remote application payloads passed exact name, length,
  and SHA-256 verification. Both directions stopped at Mapping.
- `measured-compatible`: **not achieved**. SC-001 through SC-003 remain false
  and must not be claimed.

## Evidence

Canonical standalone result:

```text
results/spec117-svs-pubsub-payload-standalone-20260716_174956/
status                    INTEROP_INCOMPATIBLE
elapsedSeconds            13.071
verified/expected         0/8
corpus manifest SHA-256   21ce01a7d876a4dd0889ee71b918027d8c90e71793d2f38375f0bd22e4538cf6
summary SHA-256           023f4d637f5f0cfb8d0b0433a238e4ac02d90c8df54424806325550714395ef1
```

The raw run also reported callbacks for C++'s own local publications. The
independent oracle correctly rejected them as unexpected rather than allowing
them to satisfy remote receipts. The peer was subsequently hardened to ignore
local callbacks, and the TypeScript peer was hardened against late asynchronous
errors after its JSONL descriptor closes. Those harness-only changes were
rebuilt/type-checked but, under the failed-standalone no-retry rule, were not
used to overwrite or relabel the measured result.

MiniNDN admission result:

```text
results/spec117-svs-pubsub-minindn-gate-20260716_175326/
status                    NOT_ADMITTED
miniNdnLaunched           false
loss00/loss05             NOT_ADMITTED / NOT_ADMITTED
summary SHA-256           33372a450fb52e81c0ed91fc18fee839fd43ae508f09524b7ef1aa4d83ae86c2
```

The launcher wrote the stop receipt before importing or starting MiniNDN. No
0% or 5% cell was consumed.

## Measured protocol boundary

The local C++ Mapping producer/query is bootstrap-session aware:

- `mapping-provider.cpp:230-239` forms
  `/<node>/<sync-prefix>/MAPPING/<bootstrap>/<low>/<high>`;
- `mapping-provider.cpp:53-74,96-104` encodes each MappingEntry with a nested
  `SeqNoEntry(BootstrapTime, SeqNo)`.

The pinned NDNts SVS-PS implementation uses the published sequence-only form:

- `subscriber_node.js:104-119` asks
  `/<node>/<sync-prefix>/MAPPING/<low>/<high>`;
- `mapping-entry_node.js:6-21` encodes `MappingEntry(SeqNo, Name)`;
- `publisher_node.js:98-121` accepts exactly the two sequence components after
  `MAPPING`.

Consequently, the NDNts Mapping Interests for C++ expired, while C++ could not
consume the NDNts Mapping form. Outer publication naming and reassembly were
not reached, so this run cannot evaluate their compatibility.

## Verification completed

```text
Python corpus/oracle/admission tests     7/7 passed
C++ peer build                           passed
TypeScript strict type-check             passed
Pinned npm install                       120 packages, 0 vulnerabilities
Existing standalone StateVector matrix   5/5 passed, separate artifacts
Canonical Spec 114/115 MiniNDN sync       6/6 SUCCESS, separate artifacts
NDN-SVS source tree                       clean at 70e682f8500e2ad205ec17722287ea0b8bd6a9f0
```

Post-run harness identities are recorded for future execution:

```text
C++ peer SHA-256        8f93357f48cddf72e26135e8b3789f01ac55619e47c4cfe8ef11cb4f2e8b19b5
TypeScript SHA-256      59a7fba6c63f4dd9a1f1c7cfed01cbb907d87a1c183a0f96f1cee929ae4434ea
package-lock SHA-256    4a18d616980fc4aa0bb0e251f6b241cd04fb8e5565c3a102066e4d9d47f45209
```

No Spec 117 source, dependency, or test file was added to or changed in the
NDN-SVS repository.

## Next owner and re-entry condition

The next work belongs in a separately audited NDN-SVS SVS-PS protocol repair,
not in this NDNSF interoperability harness. It must choose and test one
documented wire contract for Mapping and outer publication names, including
the session/re-bootstrap behavior that motivated bootstrap time. After that
repair passes NDN-SVS unit vectors, run the Spec 117 standalone gate once with
a fresh immutable result directory. Only 8/8 exact receipts may admit the 0%
and 5% MiniNDN cells.
