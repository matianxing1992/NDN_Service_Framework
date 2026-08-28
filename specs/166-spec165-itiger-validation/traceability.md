# Traceability: Spec 166

| Requirement | Task | Success criterion | Current evidence |
|---|---|---|---|
| FR-001 | T003 | SC-006 | TigerCluster job `181068`; remote materialization evidence |
| FR-002 | T004 | SC-006 | `/project/tma1/ndnsf-di/inputs/spec166/checksums.sha256` |
| FR-003, FR-005 | T005 | SC-001, SC-003 | Standalone job `181085`: 8/8 exact rows and three profile-policy-v2 PASS profiles |
| FR-004 | T006 | SC-002 | Job `181096`: `itiger07-09` and three distinct RTX 5000 UUIDs |
| FR-006 | T006 | SC-005 | Job `181096`: real NDNSF-DI flow and one identical 64-request-ID set on every Stage |
| FR-007 | T005, T006, T007 | SC-001, SC-004 | Jobs `181085` and `181096`: 8/8 exact generations; full answers and timing distributions retained |
| FR-008 | T005, T006 | SC-006 | Slurm state and progress markers monitored; immutable 10/30-minute walltimes remained terminal bounds |
| FR-009 | T005, T006 | SC-006 | Every failed job and source retained under a unique identity; accepted jobs `181085` and `181096` were separately submitted once |
| FR-010 | T003, T004, T007 | SC-006 | SIF/input/final evidence checksum-valid; failures retained as `.partial`; accepted evidence promoted only after PASS |
