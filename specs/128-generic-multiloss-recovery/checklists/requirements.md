# Spec 128 Requirements Quality Checklist

**Purpose**: Review the completeness, clarity, consistency, and measurability
of the generic multi-loss recovery and bounded-retry specification before
implementation planning.

**Created**: 2026-07-20

## Requirement Completeness

- [x] CHK001 Are bounded retry eligibility, budget, horizon, and terminal
  states all specified? [Completeness, Spec FR-002..FR-005]
- [x] CHK002 Are the distinct one-item/no-FEC and multi-segment/multi-loss
  recovery paths both specified without adding workload identity? [Completeness,
  Spec US1, US2, FR-001, FR-007, FR-008]
- [x] CHK003 Are recovery declaration membership, integrity, capacity, and
  expiry requirements documented? [Completeness, Spec FR-006]
- [x] CHK004 Are security, lifecycle, forward-progress, and resource-cap
  boundaries specified for retries and reconstruction? [Completeness, Spec
  FR-004, FR-009, FR-010, FR-013]

## Requirement Clarity and Consistency

- [x] CHK005 Is the difference between an issued retry, a valid cached/retained
  satisfaction, and a provider-confirmed future hit explicitly defined?
  [Clarity, Spec FR-011, FR-012]
- [x] CHK006 Is the capacity-plus-one case unambiguously fail-closed rather
  than a second opportunity to tune recovery? [Clarity, Spec US2, FR-010,
  SC-004]
- [x] CHK007 Are the generic-ownership exclusions consistent across stories,
  requirements, success criteria, and assumptions? [Consistency, Spec US4,
  FR-001, FR-005, FR-021, SC-009]
- [x] CHK008 Is backward compatibility for existing no-FEC and one-loss streams
  specified without conflicting with the new opt-in capacity requirement?
  [Consistency, Spec FR-007, FR-008, Assumptions]

## Acceptance and Evidence Quality

- [x] CHK009 Are the delivery, latency, useful-work, retry-bound, safety, and
  aggregate acceptance thresholds measurable for each workload family?
  [Measurability, Spec SC-002..SC-008]
- [x] CHK010 Does the evidence contract retain all failed/invalid cells and
  distinguish reliability aggregates from deterministic safety cells?
  [Completeness, Spec FR-016, FR-017, SC-007, SC-008]
- [x] CHK011 Is the immutable Spec 127 baseline identity, preservation rule,
  and non-repetition rule explicit? [Completeness, Baseline Evidence, Spec
  FR-019, SC-010]
- [x] CHK012 Are claim limits specified so the final report cannot infer
  population reliability, wireless performance, or untested workload
  generality? [Coverage, Spec FR-020, SC-011]

## Edge-Case Coverage

- [x] CHK013 Are retry/deadline, Nack, validation, stale-session, stop, and
  cached-satisfaction boundary cases specified? [Coverage, Edge Cases,
  FR-014]
- [x] CHK014 Are out-of-order repair, expired material, exact-capacity, and
  capacity-plus-one cases specified? [Coverage, Edge Cases, US2, FR-010]
