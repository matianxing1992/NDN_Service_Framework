# Research: Cross-Application Stream Generality

## Decision 1: Use absolute utility gates, not a false demand-driven baseline

**Decision**: Judge useful prefetch through Provider-confirmed future Interests,
future-hit ratio, Payload overhead, Mapping novelty, continuity, and exact
latency for each workload independently.

**Rationale**: Current source enforces Mapping v2 if and only if the policy is
`AdaptiveSampleAtomic`. `MappedLiveFutureOff` is a Mapping v1 policy. A paired
comparison would change both Mapping contract and scheduling model; adding a
new public experimental mode would be unnecessary product surface.

**Alternatives considered**: Mapping v1 future-off (confounded); a new Mapping
v2 no-prefetch policy (out-of-scope public behavior); offline counterfactual
latency (estimated rather than executed).

## Decision 2: Reuse signed sample classes as opaque application hints

**Decision**: The sensor declares one one-item class. The variable workload
declares four opaque classes capped at 1, 2, 4, and 8 source items and varies
actual extent slightly below each cap.

**Rationale**: Mapping v2 defines class ID as an opaque scheduling hint, not an
authorization or content type. Core neither knows nor parses its meaning.
Conservative caps exercise sample-atomic scheduling and terminal suffixes
without making pathological overfetch the dominant workload property.

**Alternatives considered**: one class alternating 1 and 8 segments (known
max-history overprediction stress); exact-size class per sample (no within-class
extent variation).

## Decision 3: Freeze comparable offered load

**Decision**: Both workloads publish at 10 Hz after a five-second warm-up. The
sensor emits one 256-byte source; variable samples use up to eight 4096-byte
sources plus one XOR repair. Exactly 600 measured samples are expected per run.

**Rationale**: Matching cadence makes continuity windows interpretable while
retaining intentionally different packet demand. Variable average load remains
within accepted aggregate Interest capacity and below the proven UAV item rate.

**Alternatives considered**: 30 Hz (unnecessary capacity stress); different
per-workload cadences (divergent stall/latency interpretation).

## Decision 4: Use the existing Spec 126 combined impairment

**Decision**: Each workload has one zero-loss run and five repetitions of the
same 1% loss plus bounded-reorder profile accepted in Spec 126.

**Rationale**: This isolates workload shape inside a measured fault boundary
and retains the established 4/5 engineering rule with exact intervals.

**Alternatives considered**: zero-loss only (insufficient robustness evidence);
all four Spec 126 profiles (unnecessary 32-cell expansion); physical wireless
(out of scope while algorithm development is MiniNDN-first).

## Decision 5: Keep Core and UAV source outside planned edits

**Decision**: Implement fixtures, harness, analyzer, tests, and evidence only.
Core, bindings, and UAV applications are compatibility/audit inputs.

**Rationale**: Spec 126 passed the generic correctness boundary. The new
question is whether other applications can use it without special logic.

**Alternatives considered**: proactive tuning before measurement (outcome-
driven); UAV reuse with synthetic payloads (does not prove a non-UAV boundary).
