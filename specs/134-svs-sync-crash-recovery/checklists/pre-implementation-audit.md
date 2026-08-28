# Spec 134 Pre-Implementation Audit — Revised Design

## Verdict

`CONDITIONAL PASS`

The source-contract and document correction passes. Network qualification
remains blocked until T003 provides fail-first source contracts and a corrected
single-I/O-thread driver. No NDN-SVS library repair is authorized.

## Findings

| ID | Severity | Dimension | Finding | Resolution |
|---|---|---|---|---|
| A-001 | HIGH | Intent/code reality | Previous plan equated an example comment with a README/API/test-backed cross-thread contract. | Source matrix now separates all evidence levels. |
| A-002 | HIGH | Necessity/ownership | Previous T004 patched the old library to support a disputed model instead of measuring its serial architecture. | Repair path removed; clean subject required. |
| A-003 | HIGH | Evidence integrity | Cross-thread sanitizer findings were about to be generalized to normal old-version operation. | Findings retained but scoped to the cross-thread contract gap. |
| A-004 | HIGH | Validation | Existing qualification driver directly publishes from main while Face runs elsewhere. | T003 must add a new driver; old driver remains frozen diagnostics. |
| A-005 | MEDIUM | Measurement | An I/O-thread timer can catch up in a burst if past deadlines are repeatedly armed. | Absolute release contract skips/counts elapsed slots and keeps one outstanding timer. |
| A-006 | MEDIUM | Correctness | PubSub may deliver local publications to the same subscription. | Receipt contract excludes own-sender deliveries from remote/invalid counters. |

## Code Reality

- Exact base commit: `a9944019f76791773604999f00128057b9534ace`.
- README: no threading statement.
- Public `SVSPubSub` header: no threading statement.
- Examples: explicit Face thread plus application-thread publication and
  thread-safe comment.
- Tests: no cross-thread coverage.
- Runtime: measured races under the old Spec 133/134 cross-thread drivers.
- Later commits: explicit Face/io_context serialization for worker results.

## Readiness

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Correct old serial architecture |
| Architecture and ownership | Yes | One io_context owner per peer |
| Security/correctness | Yes | Signing/wire behavior unchanged |
| Task executability | Yes | Exact paths and gates |
| Task cohesion | Yes | Four behavioral outcomes |
| Validation/evidence | Conditional | T003 driver/tests absent |
| Migration/rollback | Yes | No active NDN-SVS edit |
| Code reality | Yes | Source matrix recorded |

## Gate

T003 may implement the corrected harness. T004 MiniNDN qualification and all
Spec 133 formal execution remain blocked until T003 tests pass and the clean
subject manifest proves no repair/profiling patch is loaded.
