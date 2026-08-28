# Implementation Plan: Generic Multi-Loss Recovery and Bounded Future-Interest Retry

**Branch**: `Experimental` | **Date**: 2026-07-20 | **Spec**: [spec.md](spec.md)

## Summary

Close the two measured Spec 127 boundaries in the generic live-stream Core:
bounded exact-name future-Interest retry for one-item no-FEC samples, and a
capacity-declared opaque multi-erasure contract for multisegment samples. Keep
Mapping v2, semantic names, security, existing no-FEC/one-XOR behavior, and
application-neutral ownership compatible. Verify with deterministic tests,
binding parity, and a fresh 16-cell, one-shot MiniNDN campaign.

## Technical Context

**Language/Version**: C++17 Core; Python 3 bindings and fixtures

**Primary Dependencies**: ndn-cxx; NFD/MiniNDN; Linux tc netem; Mapping v2;
SciPy exact intervals

**Storage**: bounded in-memory cursor/group state; immutable JSON/CSV/hash evidence

**Testing**: Boost.Test; Python unittest; security contract; 60-second MiniNDN cells

**Target Platform**: Linux/MiniNDN only

**Project Type**: C++ framework with Python bindings

**Performance Goals**: impaired delivery >=99.9% one-item and >=99% multisegment;
future hits >=95%; Payload overhead <=25%; capacity-plus-one fail closed

**Constraints**: no UAV/codec/payload/workload branches; bounded retries and
in-flight work; full 16-cell fresh campaign; no automatic reruns; Spec 127 read-only

**Scale/Scope**: two opaque 10-Hz workload families, 600 samples/cell,
2 zero-loss guards + 10 impaired acceptance + 4 capacity-plus-one safety cells

## Constitution Check

- Canonical runtime: PASS; retain generic Mapping v2 live-stream surface.
- Security data path: PASS; retry/recovery preserve names, signer/session,
  Mapping, digest, validator, lifecycle and stop fencing.
- CodeGraph first: PASS; timeout skip, retry, repair, recovery, binding and
  runner paths were traced.
- Spec-driven durable work: PASS; this plan/tasks/evidence own Spec 128.
- Right-scope verification: PASS; deterministic gates precede MiniNDN.
- Cohesive tasks: PASS; retry, erasure recovery, evidence and closure are
  behavioral units.
- GSD / ARS: PASS; health is valid and experiment-plan.md freezes design.

## Design

### 1. Bounded generic retry

Each cursor retains immutable name, attempt count, retry deadline, generation,
and terminal disposition. A future timeout reschedules the same exact name only
while active, Mapping/session-valid, schedulable, before horizon, and below a
fixed generic attempt cap. Otherwise it emits exactly one terminal reason and
releases later work. Validation failure stays terminal. Initial and retry work
are separately observable; no payload, sample-class label, application, service,
codec or UAV field influences the decision.

### 2. Capacity-declared opaque multi-erasure recovery

Extend FEC metadata from a single XOR repair to signed, indexed repair symbols
with scheme and declared capacity. Implement generic GF(256) systematic parity
with two independent repair symbols, enabling any two missing opaque source
items in a group to be reconstructed. Validate group source names/cursors,
lengths, digests, repair indices, session/mapping identity and expiry first.
Keep existing None and XOR-one definitions byte-compatible and opt in only via
the generic stream declaration.

### 3. Atomic group recovery

Store validated repair symbols per group and index. Recovery selects sufficient
independent repair symbols, reconstructs all missing sources, verifies length
and digest for every result, and performs normal application admission once.
Capacity overflow, missing/invalid repair, expiry, late response, or callback
rejection is fail-closed and advances later eligible work without partial data.

### 4. Binding and evidence

Add retry initial/retry/future-hit/suppression and declared/recovered source
counters to status with pybind/Python parity. Extend only neutral fixtures and
the analyzer. Freeze a 16-cell `results/spec128-*` manifest; require unique
paths, single writer, qdisc proof, full traffic scope, baseline hashes and
one invocation per cell. Report exact intervals only for five-repeat groups.

### 5. Compatibility and rollback

The change is additive: old no-FEC and one-XOR streams remain valid. Old
consumers reject unknown repair schemes safely. Reverting removes new scheme,
counters and retry behavior only; Mapping v2 and all evidence stay unchanged.

## Project Structure

```text
ndn-service-framework/Stream.{hpp,cpp}
pythonWrapper/src/ndnsf/_ndnsf.cpp
pythonWrapper/ndnsf/streaming.py
tests/unit-tests/stream.t.cpp
tests/python/test_ndnsf_core_streaming.py
tests/python/test_ndnsf_live_stream_generality.py
tests/python/test_spec128_generic_recovery_runner.py
examples/python/live_stream/
Experiments/NDNSF_LiveStream_Generality_Minindn.py
Experiments/run_spec128_generic_recovery_matrix.py
specs/128-generic-multiloss-recovery/
```

**Structure Decision**: Extend existing Core/binding/neutral fixture owners;
no application runtime, wire namespace, or workload mode is added.

## Complexity Tracking

GF(256) parity is required because the measured one-XOR contract cannot recover
two arbitrary missing sources; repeated XOR and application specialization do
not satisfy the generic capacity requirement.
