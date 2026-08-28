# Spec 164 Requirement, Task, and Evidence Traceability

This compact matrix groups requirements that share one behavior and acceptance
gate. It does not weaken individual requirement text in `spec.md`.

| Requirement group | Behavioral contract | Primary tasks | Tests/evidence | Success criteria |
|---|---|---|---|---|
| FR-001, FR-002, FR-003, FR-004, FR-005, FR-006, FR-007 | Immutable generic artifact identity; simple, async, and resumable idempotent API | T002, T007, T008 | artifact type/API/session Python suites; `t008-single-replica-e2e.md` | SC-001, SC-009 |
| FR-008, FR-009, FR-010, FR-011, FR-012 | NDNSF Collaboration control, bounded operations, segmented Data, adaptive transfer, replica results | T006, T022–T024, T026–T031, T034–T035 | collaboration backend tests; real RepoNode public-network integration; public MiniNDN smoke; all frozen campaigns | SC-002–SC-005, SC-011, SC-012 |
| FR-013, FR-014, FR-015, FR-016, FR-017, FR-018, FR-019, FR-020, FR-021, FR-022 | Signed-root trust, hierarchical digests, bounds, downgrade/revocation, full-object integrity | T005, T008 | native/Python manifest and corruption suites; `t008-single-replica-e2e.md` | SC-001, SC-005 |
| FR-023, FR-024, FR-025, FR-026, FR-027, FR-028, FR-029, FR-030, FR-031 | Separate payload/metadata stores; streaming persistence; queued lifecycle; atomicity; recovery; ownership; deduplication | T003, T009–T011, T022, T023, T034–T035 | lifecycle/store/recovery/GC suites; real RepoNode CAS commit; `t011-minindn-recovery.md`; T023 and Phase 12 evidence | SC-005–SC-008 |
| FR-032, FR-033, FR-034, FR-035, FR-036 | Exact-packet compatibility; format negotiation; mixed version; migration/rollback; existing NDNSF authorization | T012–T015 | compatibility, migration, rollback, security suites; T014/T015 evidence | SC-010, SC-012 |
| FR-037, FR-038, FR-039, FR-040, FR-041, FR-042, FR-043, FR-044 | Canonical metrics; matched frozen campaign; sizes/replicas/concurrency; retention; MiniNDN before TigerCluster | T016–T020, T023–T032 | campaign manifests/ledgers/analyzers; canonical pointer; T029 statistical audit; final audit | SC-002–SC-007, SC-011, SC-012 |

## Remediation trace

```text
User correction: ACK must not reserve or lock storage
  -> FR-027 queued lifecycle and FR-030 bounded task queues
  -> T022/T023
  -> ReplicaTaskCollaborationClient + begin_assigned_task()
  -> public Collaboration smoke + lifecycle/contract suites
  -> SC-004 PASS

Measured high-concurrency NO_ROUTE and first-Interest timeout
  -> FR-011 bounded reliable adaptive transfer
  -> T026/T029 diagnosis, T027/T030 corrections, T028/T031 frozen campaigns
  -> failure-focused r1/c4 regression + r1/c16 recovered-timeout diagnostic
  -> fourth confirmatory campaign 144/144 PASS
  -> SC-003 PASS before TigerCluster permit

TigerCluster smoke exposed a public-network integration gap
  -> T034/T035
  -> one collaboration and one segmented producer per whole artifact
  -> queued RepoNode fetch, CAS verification, atomic commit, durable receipt
  -> public MiniNDN confirmation before a new TigerCluster attempt
```
