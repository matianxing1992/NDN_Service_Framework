# Feature Specification: Cross-Application Stream Generality

**Feature Branch**: `Experimental`

**Created**: 2026-07-20

**Status**: Complete (negative measured generality verdict)

**Input**: Define the next formal feature after Spec 126 to verify that the
accepted live-stream prefetch mechanism is useful beyond the UAV application.
Use a periodic sensor stream and a variable-size, multisegment opaque stream;
measure speed, continuity, robustness, and unnecessary Interest work without
introducing UAV-, codec-, or application-specific Core logic.

## User Scenarios & Testing

### User Story 1 - Consume a Periodic Sensor Stream (Priority: P1)

As an application developer, I can publish and consume periodic sensor samples
through the generic live-stream interface and obtain timely, ordered readings
without teaching the framework what the readings mean.

**Why this priority**: A small, regular, non-media workload is the clearest
test that prefetch decisions are driven by generic stream observations rather
than video assumptions.

**Independent Test**: Run a fixed-rate opaque sensor workload for a measured
60-second interval and verify complete ordered delivery, bounded delay, useful
prefetch, and bounded Interest work.

**Acceptance Scenarios**:

1. **Given** signed periodic samples with stable cadence and small payloads,
   **When** generic prefetch is enabled, **Then** readings arrive once and in
   publication order without an application-specific framework mode.
2. **Given** the frozen workload, **When** the run completes, **Then** Provider
   evidence proves whether Interests actually arrived before payload creation,
   and the result is rejected if proactive work is absent or mostly useless.
3. **Given** bounded loss and reordering within the Spec 126 impairment class,
   **When** readings are delayed or retried, **Then** later usable readings
   continue and every skip, retry, timeout, and Nack remains observable.

---

### User Story 2 - Consume Variable-Size Multisegment Samples (Priority: P1)

As an application developer, I can stream opaque samples whose sizes change
over time and span multiple segments, while the generic prefetch mechanism
adapts without inspecting application content or relying on a fixed sample
shape.

**Why this priority**: Variable sample size is the principal boundary for
showing that the sample-atomic predictor and Mapping control are reusable
outside a fixed video encoding profile.

**Independent Test**: Publish a preregistered, seeded sequence containing
single- and multisegment samples across at least four size classes. Verify
sample-atomic delivery, adaptation after transitions, bounded overfetch, and
forward progress under the same network profiles used for the sensor stream.

**Acceptance Scenarios**:

1. **Given** an opaque sample-size sequence that repeatedly crosses segment
   count boundaries, **When** the consumer prefetches it, **Then** only complete
   authenticated samples reach the application and segments remain ordered.
2. **Given** a size-class transition, **When** predicted and actual extents
   differ, **Then** the predictor trains once from the authenticated complete
   group, remains inside the signed class bound, and reports the prediction
   error without content parsing or a workload-specific Core hint.
3. **Given** one source segment is unavailable but valid generic repair is
   timely, **When** recovery succeeds, **Then** the complete original opaque
   sample is delivered once; otherwise the sample fails closed and later
   samples continue.

---

### User Story 3 - Compare Prefetch Utility Across Applications (Priority: P2)

As a framework maintainer, I receive one machine-readable comparison that uses
the same definitions for sensor and multisegment streams and separates useful
Payload work, Mapping work, retry work, and network failures.

**Why this priority**: Generality requires a shared evaluation contract, not
two application demos with incomparable counters.

**Independent Test**: Analyze all frozen runs with one schema and verify that
each run reports delivery delay and continuity together with Payload Interests,
Mapping Interests and Data, Mapping Data carrying new information, retries,
timeouts, and Nacks.

**Acceptance Scenarios**:

1. **Given** either workload, **When** a run finishes or fails, **Then** the
   same analyzer emits all required fields and an explicit admissibility
   verdict.
2. **Given** a Mapping Data response, **When** its utility is classified,
   **Then** the report distinguishes a response that advances known Mapping
   information from one that repeats already known information.
3. **Given** a planned repetition fails, **When** the campaign closes, **Then**
   it remains in the aggregate and is never selectively replaced.

---

### User Story 4 - Preserve Generic Ownership (Priority: P3)

As a framework maintainer, I can add both workloads without changing the
accepted Spec 126 wire, security, recovery, or scheduling contracts and without
adding sensor, UAV, codec, or payload-type branches to Core.

**Why this priority**: A benchmark cannot prove generality if it obtains its
result through hidden specialization.

**Independent Test**: Audit Core decisions and public configuration inputs,
run existing compatibility/security tests, and reject the feature if any
prefetch decision depends on application identity, service name, codec, or
payload contents.

**Acceptance Scenarios**:

1. **Given** the two application workloads, **When** their adapters are removed
   from review, **Then** Core contains no workload-specific names, parsers,
   thresholds, or policy branches introduced by Spec 127.
2. **Given** retained Spec 125 and Spec 126 evidence, **When** Spec 127 runs,
   **Then** those directories remain unchanged and are not reused as controls.

### Edge Cases

- A periodic sample arrives after the next nominal publication time.
- The sensor cadence pauses and resumes without changing stream identity.
- Consecutive multisegment samples alternate between the smallest and largest
  preregistered size classes.
- A sample extent changes at a Mapping block boundary.
- The last predicted segment is not produced, or an unexpected extra segment
  is authenticated by a later Mapping update.
- Mapping Data is valid but contains no information newer than the consumer's
  current frontier.
- Payload Data arrives while the Mapping needed to resolve it is delayed.
- Loss and reordering coincide with a sample-size transition.
- A stream stops with Mapping, payload, retry, or validation work pending.
- A malformed, stale-session, replayed, wrong-signer, or conflicting-name item
  arrives during either workload.

## Requirements

### Functional Requirements

- **FR-001**: Both workloads MUST use the same public live-stream publication,
  subscription, Mapping, security, retry, recovery, and status contracts.
- **FR-002**: Core prefetch decisions MUST depend only on generic authenticated
  stream observations and configuration, never on application identity,
  service name, sensor field, codec, media frame type, or payload contents.
- **FR-003**: The periodic workload MUST publish opaque samples at a frozen
  cadence for a measured 60-second interval and expose publication identity,
  order, and timing needed for exact delivery attribution.
- **FR-004**: The variable-size workload MUST use a preregistered seeded sample
  sequence with at least four segment-count classes, including one, two, four,
  and eight or more segments.
- **FR-005**: Multisegment samples MUST be admitted atomically: no incomplete,
  mixed-session, duplicate, or out-of-order sample may reach the application.
- **FR-006**: Size prediction MUST learn only from authenticated generic Mapping
  and delivery outcomes, with one training event per completed sample group.
- **FR-007**: Both workloads MUST preserve bounded exact-name retry, recovery,
  usefulness deadlines, stop fencing, and forward progress established by
  Spec 126.
- **FR-008**: Every run MUST separately report Payload Interests, necessary
  source-plus-selected-repair items, Mapping Interests, Mapping Data responses,
  Mapping Data responses carrying new information, retries, timeouts, and
  Nacks.
- **FR-009**: Mapping new-information ratio MUST use the same explicit formula
  in every workload: new-information Mapping Data responses divided by all
  accepted Mapping Data responses.
- **FR-010**: Every run MUST report ordered complete samples, duplicate or
  partial deliveries, skips by reason, maximum stall, future-hit ratio, exact
  publication-to-delivery latency, and measurement-window coverage.
- **FR-011**: Both workloads MUST use matched duration, topology, impairment,
  security, logging, warm-up, and measurement-window rules so their independent
  absolute acceptance results use the same evaluation contract.
- **FR-012**: The evaluation MUST include zero-loss and one bounded
  loss-plus-reordering profile already within the Spec 126 fault model; it MUST
  NOT tune a workload or network profile after the first frozen run begins.
- **FR-013**: Commands, source identity, workload sequence, configuration, and
  result directory MUST be frozen before execution; each planned cell runs
  once and failures remain measured outcomes without automatic replacement.
- **FR-014**: A shared analyzer MUST apply identical counter definitions and
  acceptance rules to both workloads and MUST retain metrics from failed runs.
- **FR-015**: Historical Spec 125 and Spec 126 evidence MUST remain byte-for-byte
  unchanged and MUST NOT serve as repetitions for the new code revision.
- **FR-016**: Existing wire encoding, semantic names, validation, authorization,
  replay protection, public APIs, and accepted zero-loss/loss-reorder behavior
  MUST remain compatible.
- **FR-017**: No UAV controller, camera, video decoder, codec parser, media
  frame classification, application-name heuristic, new wire version, or
  application-selectable prefetch mode is in scope.
- **FR-018**: A positive generality claim MUST require both workloads to pass
  independently; one passing workload MUST NOT compensate for the other.
- **FR-019**: A useful-prefetch claim MUST require Provider-confirmed proactive
  Interests plus the absolute latency, continuity, and Interest-utility gates;
  it MUST be withheld when proactive evidence is absent, mostly useless,
  incomplete, or invalid. No causal comparison claim is in scope.
- **FR-020**: Deterministic tests MUST cover cadence pause/resume, size-class
  transitions, delayed Mapping, stale Mapping, stop races, and malformed or
  replayed Data before a live campaign begins.

### Key Entities

- **ApplicationNeutralSample**: Opaque bytes plus authenticated publication
  identity and sample boundaries, with no framework-visible application type.
- **PeriodicSensorWorkload**: Frozen cadence, opaque sample generator, duration,
  and expected publication sequence.
- **VariableMultisegmentWorkload**: Seed, ordered sample-size classes, segment
  extents, and expected complete-sample sequence.
- **PrefetchEvaluationCell**: Workload, network profile, repetition identity,
  frozen command, proactive-utility evidence, and one-shot status.
- **TrafficUtilityReport**: Payload necessity and overhead, Mapping response
  novelty, retry/timeout/Nack outcomes, future hits, continuity, and latency.
- **GeneralityVerdict**: Per-workload correctness and efficiency verdict plus
  the rule that both independent workloads must pass before a shared claim.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Across deterministic tests for both workloads, application
  delivery contains zero duplicates, zero partial samples, zero out-of-order
  samples, and zero post-stop mutations.
- **SC-002**: In every accepted zero-loss periodic run, at least 99.9% of
  expected readings are delivered in order, the final ten seconds contain no
  stall longer than two publication periods, and publication-to-delivery p95
  is no greater than two publication periods.
- **SC-003**: In every accepted zero-loss variable-size run, at least 99% of
  expected samples are delivered completely, all four size classes are
  observed, the final ten seconds contain no stall longer than one second, and
  publication-to-delivery p95 is at most 250 ms and p99 at most 500 ms.
- **SC-004**: For each workload under zero loss, at least 99% of prefetch
  Payload Interests are satisfied by future Data, Payload Interest work is no
  more than 15% above necessary source-plus-selected-repair items, and at least
  90% of accepted Mapping Data responses advance known Mapping information.
- **SC-005**: For each workload under the bounded combined impairment, at least
  four of five frozen prefetch repetitions complete with no lifecycle failure
  or final-window stall beyond that workload's limit; successful repetitions
  have at least 95% future hits and no more than 25% Payload Interest overhead.
- **SC-006**: Every run counted as successful contains nonzero
  Provider-confirmed future Interests, meets its workload latency and
  continuity bounds, and meets the applicable future-hit and Interest-overhead
  limits. A run with no demonstrated proactive work cannot support a useful-
  prefetch or generality claim.
- **SC-007**: Every planned run, including failures, reports non-missing values
  for Payload, Mapping, Mapping-new-information ratio, retry, timeout, Nack,
  continuity, coverage, and latency fields, or an explicit reason the field is
  unavailable.
- **SC-008**: All planned commands have one recorded invocation and unique
  output paths, automatic retry is absent, frozen source and workload hashes
  remain stable, and retained Spec 125/126 evidence hashes are unchanged.
- **SC-009**: A code-aware post-implementation audit finds zero Core branches,
  constants, parsing, or configuration keyed to sensor, UAV, codec, media, or
  either benchmark's application name.
- **SC-010**: The final report makes no broader wireless, workload-population,
  or reliability claim beyond the two tested workload families and declared
  impairment profiles.

## Assumptions

- The periodic workload uses a stable, preregistered cadence and opaque values;
  exact rate and payload size are frozen during planning rather than tuned from
  results.
- The variable-size workload uses a deterministic seed and repeatedly visits
  every declared size class so transition behavior is observable.
- A demand-driven comparison is intentionally excluded: the accepted Mapping
  v2 contract requires adaptive sample-atomic scheduling, while the existing
  future-off policy belongs to Mapping v1 and would not be a matched reference.
- MiniNDN remains the final network environment while the algorithm is under
  development. Physical devices, host NFD, Docker, iTiger, and application-
  specific acceleration are out of scope.
- Five impaired repetitions provide an engineering boundary with exact
  intervals, not a population-level reliability estimate.
- Spec 126 `confirmation07` remains frozen historical evidence and is not a
  control repetition for Spec 127.
