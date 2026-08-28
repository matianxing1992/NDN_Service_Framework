# Spec 125 Traceability

| Requirement | Owning task | Verification or evidence |
|---|---|---|
| FR-001, FR-002, FR-003 | T001 | Mapping v2 golden wire, group admission, v1/v2 isolation tests |
| FR-004 | T002 | Whole-group admission/deferral and block-crossing tests |
| FR-005, FR-006, FR-007 | T001 | Per-class predictor, cold-start, cap, isolation and rejected-observation tests |
| FR-008 | T001, T002 | Authenticated actual-extent and correction tests |
| FR-009 | T001, T002, T004 | Status/unit assertions, sampled `NDN_LOG`, frozen result |
| FR-010 | T002 | Variable-source FEC, one-source repair and digest tests |
| FR-011 | T003, T004 | UAV real access-unit tests and first-sample frozen evidence |
| FR-012 | T001, T003 | Semantic-name Mapping, Validator, AEAD and exact-name tests |
| FR-013 | T003 | APP-owned opaque key/delta policy and Core API boundary tests |
| FR-014 | T001, T002 | Class/history/capacity/pending-state bound tests |
| FR-015 | T004, T005 | Original retained cell plus uniquely named post-fix confirmations |
| FR-016 | T001, T003 | C++/Python Provider API parity and UAV caller wiring |
| FR-017 | T001, T002 | Adaptive `openLiveStream` and consumer handle tests |
| FR-018 | T001 | C++ golden vectors and 19 Python streaming tests |
| FR-019 | T001, T003 | Timing seed contract and frozen UAV bitrate/FPS/GOP setup |
| FR-020 | T002 | Complete predicted-group demand and pressure tests |
| FR-021 | T001, T002 | Impossible aggregate-cap rejection tests |

| Success criterion | Disposition | Evidence |
|---|---|---|
| SC-001 | PASS | Deterministic whole-group scheduler traces |
| SC-002 | PASS | Per-class isolation and bounded-history tests |
| SC-003 | PASS | Stream 46/46 and full C++ 333/333 |
| SC-004 | PASS | `confirm06`: real one-source groups, one repair, 1832 GUI frames |
| SC-005 | PASS | `confirm06`: independent key/delta convergence, zero underpredictions |
| SC-006 | PASS | `confirm06`: 0.463% overhead, 99.4866% future hits, p95 124.586 ms, p99 126.538 ms, continuous tail |

Canonical commands, raw logs, measurements, the retained negative result, and
accepted repair evidence are recorded in `completion-summary.md`,
`results/spec125-adaptive-sample-atomic-20260719-acceptance/`, and
`results/spec125-adaptive-sample-atomic-20260719-confirm06/`.
