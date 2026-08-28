# Implementation Plan: SVS PubSub Payload Interoperability

**Branch**: `117-svs-pubsub-payload-interop` | **Date**: 2026-07-16 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/117-svs-pubsub-payload-interop/spec.md`

## Summary

Extend the NDNSF-owned C++/TypeScript SVS V3 interop example with an explicit
SVS-PS payload mode and a deterministic four-case corpus. Each peer publishes
and subscribes through its implementation's public PubSub API, emits
machine-readable stage and payload receipts, and accepts a direction only after
the application name, byte length, and SHA-256 match. A bounded standalone gate
runs first. MiniNDN 0%/5% cells are admitted only if standalone interoperability
passes; otherwise the feature records the exact mapping/fetch/encapsulation
boundary and withholds the compatibility claim.

## Technical Context

**Language/Version**: C++17, TypeScript executed by Node.js 22+, Python 3

**Primary Dependencies**: local NDN-SVS `SVSPubSub`, pinned NDNts
`@ndn/svs` `SvPublisher`/`SvSubscriber`, ndn-cxx 0.9, NFD, MiniNDN

**Storage**: In-memory publication stores plus JSON/JSONL result artifacts

**Testing**: focused Python contract tests, TypeScript type checking/runtime
smoke, C++ build, standalone two-process NFD test, conditionally admitted
bounded MiniNDN cells

**Target Platform**: Linux local host and MiniNDN namespaces

**Project Type**: Cross-language protocol interoperability example and test
harness

**Performance Goals**: Not a throughput benchmark; every admitted cell must
terminate within a fixed deadline and account for all expected payloads

**Constraints**: No NDN-SVS source/unit-tree changes, no NDNts fork, no private
wire adapter, no route injection to transient application faces/publication
names, no rerunning a failed formal cell into a pass

**Scale/Scope**: Two implementations, two directions, four deterministic
payload cases, standalone gate, then one 0% and one 5% MiniNDN acceptance cell

## Constitution Check

*GATE: Passed before design and rechecked after design.*

- **Canonical runtime**: This is an SVS protocol example, not a new NDNSF
  service API; no generated or legacy invocation path is introduced.
- **Security path**: Shared test HMAC signers/verifiers remain enabled. Payload
  validation failures are fail-closed and cannot generate a receipt.
- **CodeGraph/source verification**: CodeGraph located the existing NDNSF-owned
  peers and actual source inspection established the current APIs and likely
  Mapping-format risk.
- **Spec-driven durable work**: Spec 117 owns the new cross-language test,
  artifacts, and bounded campaign.
- **Right validation scope**: Local contract/standalone tests precede MiniNDN;
  MiniNDN is the final network gate.
- **Cohesive tasks**: Each task closes one behavior with its test,
  implementation, and evidence. No separate per-file or per-command tasks.

Post-design recheck: passed. The design does not weaken a security or protocol
invariant and does not place downstream tooling in NDN-SVS.

## Architecture and Ownership

```text
NDNSF repository
├── deterministic corpus + receipt checker          authoritative test intent
├── C++ external consumer of NDN-SVS SVSPubSub       C++ peer behavior
├── TypeScript consumer of pinned NDNts SVS-PS       independent peer behavior
└── standalone/MiniNDN orchestration + evidence      acceptance authority

NDN-SVS repository
└── production library + unit tests                  read-only test subject

Pinned NDNts packages
└── independent TypeScript implementation            read-only test subject
```

The official SVS-PS specification defines Mapping queries as
`/<node>/<sync-prefix>/MAPPING/<low>/<high>`, MappingEntry as `SeqNo + Name`,
and outer publication names as
`/<node>/<sync-prefix>/<seq>[/v=0/seg=N]`. Current source inspection shows the
local C++ implementation may additionally bind bootstrap time into Mapping and
outer publication identifiers. That is a hypothesis to measure, not a format
the NDNSF harness may translate.

## Payload Corpus

The corpus is deterministic and generated independently from peer runtime:

| Case | Intended boundary | Required properties |
|---|---|---|
| `text` | ordinary application payload | fixed UTF-8 text including non-ASCII code points |
| `binary` | opaque payload | 1,024 bytes containing every octet value, including `0x00`, `0x80`, and `0xFF` |
| `large` | larger independently named object | 4,096 deterministic bytes |
| `segmented` | mandatory segmentation | 32,768 deterministic bytes with a 4,096-byte chunk hint; multiple segments required |

The corpus manifest is authoritative. Peers receive payload files and expected
metadata; they do not independently invent expected digests.

## Validation Flow and Stop Conditions

```text
fixture/contract checks
  -> C++ and TypeScript compile/runtime preflight
  -> bounded standalone C++ <-> NDNts payload exchange
       pass -> MiniNDN 0% -> MiniNDN 5% -> post-implementation audit
       fail -> classify protocol stage, preserve packets/logs, stop matrix
```

A StateVector update is only a progress event. Acceptance requires a subscriber
payload callback and exact receipt. Mapping timeout, malformed MappingData,
outer-name miss, inner decoding failure, signature failure, incomplete segment
set, digest mismatch, duplicate, and process exit are distinct terminal or
diagnostic outcomes.

## Project Structure

### Documentation

```text
specs/117-svs-pubsub-payload-interop/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/payload-receipts.md
├── quickstart.md
├── tasks.md
└── completion-summary.md
```

### Source and Test Surfaces

```text
examples/interop/ndn-svs-v3/
├── cpp/svs3-peer.cpp
├── ndnts/svs3-peer.ts
├── ndnts/package.json
├── payload_corpus.py
├── run-payload-standalone.py
└── README.md

Experiments/
└── NDN_SVS_PubSub_Interop_Minindn.py

tests/python/
└── test_spec117_svs_pubsub_interop.py
```

**Structure Decision**: Reuse the existing NDNSF-owned external interop
directory and add a separate payload runner so the established StateVector
runner remains unchanged and its evidence cannot be conflated with SVS-PS.

## Complexity Tracking

No constitution violation or additional compatibility layer is accepted. A
protocol mismatch becomes a measured blocker for a separately owned repair.
