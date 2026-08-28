# Implementation Plan: Stream Prefetch Retention Recovery

**Branch**: `Experimental` | **Date**: 2026-07-19 | **Spec**: [spec.md](spec.md)

## Summary

Repair the Core contract that confuses evicted Data with future Data and make `LiveStreamConsumerHandle` implement the paper's real Interest pipeline: exact future names, payload-Interest/Data DRD, chasing bursts, live-edge detection, and replacement Interest expression independent of application processing. Then make UAV video retention and access-unit assembly explicitly bounded by live duration, and validate the Core path through the original high-packet-rate UAV configuration.

## Technical Context

**Language/Version**: C++17 and Python 3

**Primary Dependencies**: ndn-cxx, NFD/MiniNDN, GStreamer, OpenSSL, GTK

**Storage**: bounded in-memory signed Data retention; existing optional DistributedRepo recording remains unchanged

**Testing**: Boost.Test unit suite, Python experiment analyzers, MiniNDN UAV GUI experiment

**Target Platform**: Ubuntu Linux / MiniNDN

**Project Type**: C++ framework plus UAV application and experiment harness

**Performance Goals**: continuous 30 FPS consumption for 60 seconds; capture-to-decode p95 <=250 ms; no >2 s delivery plateau

**Constraints**: preserve semantic names, signatures, validation, encryption, FEC, rollback, and frozen evidence; use `NDN_LOG`, not stdout hot-path diagnostics

**Scale/Scope**: one Provider and one ground station, 8 Mbps, 30 FPS, zero configured loss for the acceptance run

## Constitution Check

- Canonical dynamic runtime: PASS; no service invocation API changes.
- Security in the data path: PASS; signed Data, validation, AEAD, and replay behavior remain mandatory.
- CodeGraph first: PASS; actual publisher, consumer, fetcher, and UAV callers were traced before design.
- Spec-driven durable work: PASS; this independent feature owns the repair without rewriting Specs 121/122.
- Right-scope verification: PASS; test-first Core coverage followed by a 60-second MiniNDN UAV gate.
- Cohesive tasks: PASS; tests, implementation, and evidence for one behavior remain in the same task.

## Design

### 1. Payload lifecycle classification

`LiveStreamPublisher` already retains immutable reservations and materialization history. An Interest may enter `m_pendingPayloads` only when its reservation has never been materialized. A materialized name absent from `m_payloadPackets` is evicted, not future, and must not occupy pending capacity or increment future eligibility.

### 2. Paper-aligned Interest pipeline

`LiveStreamConsumerHandle::schedule()` treats `StreamFetchDecision` as authoritative: use its aggregate limit, payload range/budget, Mapping budget, and Interest lifetime. Each payload Interest records its own expression time; matching Data reception supplies DRD before validation or APP processing. Once Data arrives, the network slot is refilled over already validated Mapping while the received cursor remains in a separate bounded processing set. This preserves a full `lambda_p` pipeline even when validation, decryption, assembly, or decoding is temporarily slower than one callback.

The existing `Chasing`, `Adjusting`, and `Fetching` phases must affect actual in-flight payload Interests. Chasing fills a bounded burst over exact mapped names; normalized sample arrivals detect the live edge; adjusting withholds replacement slots toward `ceil(DRD/T) * itemsPerSample`; fetching maintains that demand. Capture-to-receive time remains application latency evidence and is not fed back as RTT.

For `MappedLiveFutureOn`, that decision-time Interest lifetime is also the live usefulness deadline. Expiry skips the cursor once and advances; it does not spend the recording-oriented three-attempt retry budget. `MappedPressure`/beginning playback keeps the existing retry contract.

### 3. Live retention and application recovery

UAV-APP derives a bounded retained-item count from a named retention-duration target and its declared items-per-frame/FPS, with a defensive cap. Core delivers cursor order, so the ground station keeps the simpler one-access-unit assembly; when the next frame begins before completion it emits an `NDN_LOG` drop reason, resets bounded state, and continues feeding later complete units.

### 4. Evidence

Unit tests first reproduce the publisher classification, decision-bypass, callback-gated refill, and incorrect-DRD defects. A unique Spec 123 MiniNDN run using the restored 3600-byte/12+1 contract then reports per-bucket decoded/displayed activity, frontiers, pending counts, future-hit ratio, Interest work, frame age, and queue/drop state. No failed cell is silently replaced.

## Project Structure

```text
ndn-service-framework/Stream.{hpp,cpp}
NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp
NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp
tests/unit-tests/stream.t.cpp
tests/unit-tests/uav-protocol-state.t.cpp
Experiments/NDNSF_UAV_GUI_Minindn.py
Experiments/analyze_stream_latency.py
specs/123-stream-prefetch-retention-recovery/
results/spec123-*/
```

**Structure Decision**: retain the existing Core/UAV ownership boundary. Generic lifecycle and scheduling belong to Core; video retention sizing and access-unit assembly belong to UAV-APP.

## Complexity Tracking

No constitution violation or new wire format is required.
