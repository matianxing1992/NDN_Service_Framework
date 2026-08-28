# Traceability

| Requirement / criterion | Design owner | Task | Acceptance evidence |
|---|---|---|---|
| FR-001, SC-001 | Five product architecture | T003-T006 | `evidence/build-validation.md`; manifests list five accepted images |
| FR-002 | ML Base | T003 | ML layer probe and recorded versions |
| FR-003 | Stable NDN Base | T004 | NDN probe plus ndn-svs/NDNSF absence check |
| FR-004 | App Runtime | T005 | C++/Python/native-provider static probe |
| FR-005, FR-006 | Locks and layer seals | T002 | 10/10 focused tests and lock/seal digests |
| FR-007 | Prefix ownership | T003-T005 | Filesystem ownership verifier |
| FR-008, FR-009 | Build driver and manifest | T006 | Two canonical `build-manifest.json` records |
| FR-010 | Fail-closed gates | T002-T006 | Negative tests and failed mutation cases |
| FR-011, SC-002 | Runtime user and static health | T005-T006 | UID 65532 static and read-only-root probes |
| FR-012 | Content exclusions | T002, T003, T005 | Context/image content and secret scans |
| FR-013, SC-003, SC-004 | App-only incremental reuse | T006 | `reuse-proof.json` with four common parent IDs |
| FR-014, SC-006 | Legacy rollback | T001, T006, T008 | Spec 110 runtime ID `sha256:57f3804cf787...` retained |
| FR-015 | Explicit cleanup | T007-T008 | Dry-run cleanup inventory |
| FR-016 | Evidence authority | T001, T007-T008 | Manifest/completion limitation labels |
| FR-017 | Frozen Spec 110 | T001, T008 | Scoped Git diff and evidence checksum boundary |
| FR-018, SC-005 | Operating guide | T007 | Reproduction from maintained guide |
| SC-007 | Final audit | T008 | `evidence/post-implementation-audit.md`: PASS |
