# Traceability

| Requirement | Design/contract | Task | Evidence |
|---|---|---|---|
| FR-001 | `plan.md` Source-backed Design | T002 | focused pacing/regression report |
| FR-002 | unchanged exact/API behavior | T002 | focused native regressions |
| FR-003 | deterministic/focused pacing tests | T002 | native test output |
| FR-004 | six ordered rates | T003, T004 | frozen campaign |
| FR-005 | matched factors and >=60 s | T003, T004 | cell manifests |
| FR-006 | hashes and no-rerun rule | T003, T004 | campaign manifest/lock |
| FR-007 | full metric schema | T003–T005 | cell/aggregate CSV and Markdown |
| FR-008 | acceptance thresholds | T003–T005 | campaign summary |
| FR-009 | API/session identity | T003, T004 | API markers and analyzer checks |
| FR-010 | ownership and Core prohibition | T001, T005 | pre/post implementation audits |
| FR-011 | frozen evidence | T001, T005 | before/after hashes |
| FR-012 | diagnostic/formal failure rule | T003–T005 | terminal result roots |
| SC-001 | focused pacing gate | T002 | native test output |
| SC-002 | low/high diagnostic gate | T003 | unique diagnostic roots |
| SC-003–SC-004 | six-cell formal matrix | T004, T005 | campaign summary and comparison |
| SC-005 | closure source/frozen audit | T005 | post-implementation audit |
