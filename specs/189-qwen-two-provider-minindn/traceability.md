# Spec189 Traceability

| Requirement / Story | Owning tasks | Batch | Production entry / C++ oracle | Evidence |
| --- | --- | --- | --- | --- |
| US1 / FR-001..FR-005 | T001,T002,T003 | B189-0,B189-1 | `Runtime::prepare`, `NativeCanonicalPreparationCatalog`, `DI_NativeArtifactAuthority` | `evidence/b189-prepare.md` |
| US2 / FR-006..FR-009 | T004,T005 | B189-2 | `NativeRequestEnvelope`, Core ACK/Selection, `DI_NativeRequester` | `evidence/b189-placement.md` |
| US3 / FR-010..FR-015 | T006,T007 | B189-3 | `NativeCanonicalOnnxAssembler`, `NativeOnnxAssemblyWorker`, `di-native-provider` | `evidence/b189-execution.md` |
| US4 / FR-016..FR-019 | T008 | B189-4 | native lease/runner counters plus maintained resource wrapper | `evidence/b189-resource.md` |
| US5 / FR-020..FR-024 | T009,T010 | B189-5 | C++ event oracle and evidence checker | `evidence/b189-convergence.md` |

## Functional requirement detail

| ID | Task | Production proof |
| --- | --- | --- |
| FR-001 | T001,T002 | pinned snapshot/revision and file hash manifest |
| FR-002 | T002 | native graph/initializer/range validation |
| FR-003 | T003 | Repo commit and zero request-time publication counters |
| FR-004 | T003,T004 | prepared handle reference/lease ownership inspection |
| FR-005 | T003 | duplicate prepare C++ selector |
| FR-006 | T004 | `NativeRequestEnvelope` parser oracle |
| FR-007 | T005 | Core ACK event before Selection/fetch |
| FR-008 | T005 | signed two-placement Selection fields |
| FR-009 | T005 | no-fetch/range/digest negative cases |
| FR-010 | T006 | provider package digest/range/role verification |
| FR-011 | T006 | native assembly from selected packages |
| FR-012 | T007 | production hidden-state handoff identity |
| FR-013 | T007 | C++ terminal output digest/top-token oracle |
| FR-014 | T007,T008 | cancellation/close join-drain selector |
| FR-015 | T006,T008 | runner/lease/materialization counters |
| FR-016 | T008,T009 | lifecycle resource samples |
| FR-017 | T008 | deterministic resource guard classification |
| FR-018 | T008,T010 | peak/post-drain metrics and repeat comparison |
| FR-019 | T009,T010 | full event sequence required for PASS |
| FR-020 | T001,T010 | immutable candidate tuple |
| FR-021 | T001,T009 | pre-dispatch identity rejection |
| FR-022 | T007,T009 | C++ oracle / Python orchestration boundary |
| FR-023 | T009,T010 | raw log and four miss-class evidence |
| FR-024 | T009,T010 | no SIF/Tiger dispatch |

## Five-lane coverage

Each batch evidence records:

1. **Production entry/callers** — CodeGraph and exact source paths for prepare, request, Core and Provider.
2. **Implementation/wire** — manifest/reference, ACK/Selection and placement/handoff fields.
3. **Test/harness/oracle** — named C++ target/selector; Python only launches external facilities.
4. **Build/source closure** — Waf target, source list, ABI hashes, `nm -C`, `readelf` and `ldd` checks.
5. **Migration/evidence** — immutable candidate tuple, raw logs, resource samples, verdict and first boundary.

Unknown lanes remain `gap`; static review or artifact export cannot close them.
