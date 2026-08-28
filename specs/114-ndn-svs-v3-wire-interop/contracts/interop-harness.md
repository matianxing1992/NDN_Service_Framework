# Contract: Independent Interoperability Harness

## Participants

### C++ peer

Path: `../ndn-svs/tests/interop/cpp/svs3-peer.cpp`

Required arguments:

```text
--version v2|v3
--sync-prefix <NDN name>
--node-prefix <NDN name>
--publish-count <N>
--publish-interval-ms <N>
--settle-ms <N>
--hmac-key-base64 <key>
--events <JSONL path>
```

### Independent NDNts peer

Path: `../ndn-svs/tests/interop/ndnts/svs3-peer.mjs`

It exposes the same logical arguments, uses `openUplinks()` to the node-local
NFD, enables `svs3: true`, and explicitly configures:

```text
syncInterestLifetime = 1000 ms
periodicTimeout       = [30000 ms, 0.1]
suppressionPeriod     = 200 ms
```

`package.json` and lockfile pin exact package identities. Generated
`node_modules/` is never committed.

## Event record

Each peer writes JSON Lines with:

```json
{
  "event": "startup|publish|update|reject|shutdown",
  "implementation": "cpp|ndnts",
  "protocolVersion": 3,
  "nodeName": "/node",
  "bootstrapTime": 0,
  "low": 1,
  "high": 1,
  "reason": "",
  "timestampNs": 0
}
```

Secrets are never written. The HMAC key is a fixed public test key only.

## Standalone matrix

Before MiniNDN, run on two local processes attached to NFD:

1. C++ publishes; NDNts observes.
2. NDNts publishes; C++ observes.
3. Both publish concurrently and converge.
4. C++ V2 regression against the pinned V2 fixture.
5. V2/V3 mismatch produces no update; both peers declare their selected profile
   and the harness emits the incompatibility diagnostic.

This smoke is diagnostic; MiniNDN remains the final network gate.

## Formal MiniNDN matrix

Each cell uses a unique directory and one C++ plus one NDNts peer:

| Cell | Loss | Repetition | Publications |
|---|---:|---:|---:|
| loss00-run01 | 0% | 1 | 20 each direction |
| loss00-run02 | 0% | 2 | 20 each direction |
| loss00-run03 | 0% | 3 | 20 each direction |
| loss05-run01 | 5% | 1 | 20 each direction |
| loss05-run02 | 5% | 2 | 20 each direction |
| loss05-run03 | 5% | 3 | 20 each direction |

Measured acceptance begins before the first publication and ends when vectors
converge or 60 seconds after the final publication. Coverage counts unique
sequence numbers contained in callback ranges, not callback invocations.
Setup/warmup is outside the window. Every candidate/cell pair runs exactly once.

## Packet capture

Each cell preserves a bounded sample containing at least the first C++ packet,
first NDNts packet, one re-bootstrap packet when applicable, and every rejected
packet. The validator records decoded names/TLVs and SHA-256 digests without
requiring bulk pcap retention.

## Stop conditions

- packet contract mismatch;
- missing/duplicate update;
- divergent final vector;
- peer restart or uncaught exception;
- source/binary/lockfile identity drift;
- extension state mutation from malformed input;
- formal cell already exists for the same candidate.
