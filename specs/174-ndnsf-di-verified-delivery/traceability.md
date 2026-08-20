# Planning Traceability Matrix: Spec 174

This matrix fixes requirement ownership before implementation. `Planned` means the named current owner and regression are the first reuse candidates; it is not a claim that the behavior has passed. T002 verified the owners against current source and the current local gate evidence; Tiger-only rows remain explicitly external and unexecuted.

## T002 Current-session verification record

The machine-readable map is `results/spec174/t002/traceability.json` (schema
`spec174-traceability-v1`, SHA-256
`sha256:94c255d8b4e6bb0e6fe25cff41eb56603c6d9c22fcc7d647b12d6485a433b69d`).
It contains exactly 29 functional requirements and 11 success criteria, with
one owner, regression input, delivery task, proof obligation, and explicit
status for each row. The current local closure evidence is
`evidence/local-gates-2f8aeab5.json` at Experimental commit
`2f8aeab5d7fc1f3ec8c0c1693aa3cb3b54dcf51c`. It records T004–T013 as
conforming/reused and executed through the local gates; T014–T016 remain
external Tiger stages, and T017 remains open until those stages and the final
audit are complete. No local evidence is promoted to a Tiger PASS.

## Current local closure

The local evidence manifest binds the owner map to the same source revision,
fixture identities, and exact candidate SIF. It records Gate U (527 native
cases plus 12 explicit Spec174 Python cases), Gate I (36 production cases in
three independent processes), Gate M (four assignments × three fresh
MiniNDN+CPU processes), and Gate C (one source-sealed local Apptainer SIF).
The external gate is currently `BLOCK` before promotion because the VPN/SSH
path is lossy; no Tiger workload was submitted.

## Functional Requirements

| Requirement | Current owner/reuse candidates | Existing regression input | Delivery task | Required proof |
|---|---|---|---|---|
| FR-001 | application API, `app_sdk`, normal NDNSF request | default-application-path and app API tests | T004 | request succeeds without Provider list/stage map; forbidden placement fields absent |
| FR-002 | `CollaborationAckClosure`, V3 lifecycle | ACK-closure and assignment V3 tests | T004 | exact immutable closure; late/mutated ACK rejected |
| FR-003 | `PlacementPlanCoreV3`, `NativeExecutionPlan`, placement modules | native-plan, placement/assignment V3 | T004 | bijective role/Provider map; duplicates/missing/spread rejected |
| FR-004 | placement core plus `CollectiveRuntime`/group coordinator | collective/group/hybrid tests | T004, T008 | explicit rank roles and complete sealed tensor group |
| FR-005 | `app_sdk/presplit.py`, placement policy/core | placement V3, plan sealer, hybrid bundle | T004 | deterministic pure PreSplitFirst proposal; trusted revalidation |
| FR-006 | plan sealer, NDNSF Selection projection | plan-sealer and core-flow integration | T004 | no Selection before commit; exact Provider-local projection |
| FR-007 | Selection verifier, Provider runtime, admission/protected runtime | admission, exact residency, protected runtime | T004, T005, T009 | exact local validation and fresh complete peak-vector admission |
| FR-008 | canonical artifacts, repository, ONNX adapters | canonical layers/content-addressed reuse | T005 | immutable canonical ONNX objects and graph-valid candidates |
| FR-009 | Provider handler/runtime/session, native runner, ORT backend | Provider assembly, ORT boundary, post-Selection integration | T005 | local fetch/assembly/coverage/ORT execution |
| FR-010 | ORT backend, packaging/runtime probes | ORT boundary and SIF/runtime dependency tests | T005, T013 | deployed path imports/runs without PyTorch/Transformers |
| FR-011 | Provider role worker, async dataflow runtime | async runtime, hybrid/integrated flow | T005, T008 | local dependency readiness; no global prep barrier |
| FR-012 | final result contract and normal response handling | default application/integrated flow | T006 | exactly one complete accepted application result |
| FR-013 | repository naming and dependency I/O | role-dataflow and repository tests | T007 | persistent repository names; request tensors producer-owned |
| FR-014 | `NdnsfCollaborationDependencyIo`, tensor codec, collective control | dependency evidence, cross-provider group | T007, T008 | every cross-Provider tensor observed as NDN Interest/Data; no hidden fallback |
| FR-015 | production name/codec helpers | role-dataflow/native-plan golden tests | T007 | all mandatory name bindings present and canonical |
| FR-016 | tensor manifest/segment consumer | async runtime, tensor bundle/segmentation tests | T007 | manifest-first exact validation, reorder/dedup/reconstruction once |
| FR-017 | role worker, async runtime, collective runtime | hybrid execution/native bundle | T008 | one scheduler; computed merges are explicit roles |
| FR-018 | normal NDNSF permission/NAC-ABE/token/signature/replay owners | security routing/token/replay regressions | T009 | security preserved across request through tensor and response |
| FR-019 | `ProtectedRuntime`, `ProviderGroupCoordinator`, plan security | protected runtime, group capability, artifact security | T009 | exact grant/capability/key binding before ORT plaintext |
| FR-020 | recovery/state/session/worker/group/protected owners | recovery, cancellation, async/group tests | T006, T008, T009 | bounded fencing/replan/restart and terminal zeroization |
| FR-021 | canonical/session/cache/reuse owners | reuse/residency/runtime compatibility tests | T005, T006, T009 | exact compatibility or clean fallback/new plan; unchanged output semantics |
| FR-022 | current evidence/release-gate/runtime journal owners | dependency/evidence bundle tests | T003, T010–T017 | bounded non-secret lifecycle and first-failure manifests |
| FR-023 | current C++/Python unit suites | distributed-inference and Spec170 unit regressions | T010 | complete unit coverage/pass manifest |
| FR-024 | core-flow integration and shared fixture | `ndnsf-di-core-flow.t.cpp` | T011 | production four-Provider lifecycle and full fault matrix, 3 processes |
| FR-025 | LLM pipeline MiniNDN and real-gate harness | current real MiniNDN gate | T012 | real processes/per-node NFD pipeline/tensor/hybrid + oracle, 3 runs |
| FR-026 | local release gate, SIF builder/validators | exact-SIF and build-boundary tests | T013, T014 | no Tiger admission before U/I/M/C identity-bound PASS |
| FR-027 | Tiger preflight/runner/templates/finalizer | current container/Tiger unit/integration tests | T014–T016 | T0–T3 staged stop, unchanged candidate, complete answers |
| FR-028 | API/operator config and placement policy boundary | API/public policy and placement tests | T004 | policy/bounds configurable; request-specific placement cannot be predeclared |
| FR-029 | all current owners plus CodeGraph/traceability process | current regressions named above | T002, T017 | every change reuses or explicitly justifies migration/new owner; no parallel subsystem |

## Success Criteria

| Criterion | Closing task(s) | Evidence |
|---|---|---|
| SC-001 | T002, T017 | 29/29 FR and 11/11 SC mapped; PDF reconciliation; no Spec170 task dependency |
| SC-002 | T010 | Gate U PASS; required rejections have zero output/authority |
| SC-003 | T011 | Gate I pipeline/tensor/hybrid/fault cases, three fresh processes |
| SC-004 | T012 | Gate M exact edge/oracle evidence, three clean runs per required case |
| SC-005 | T010–T012 | bounded negative cases with first failure and no accepted result |
| SC-006 | T013, T014 | promotion guard consumes exact U/I/M/C PASS chain |
| SC-007 | T014 | local and remote SIF hashes match; no remote rebuild/library replacement |
| SC-008 | T015 | complete single-GPU oracle, intended ORT provider, zero CPU fallback |
| SC-009 | T016 | cross-Provider rank roles, exact NDN edges, complete GPU oracle |
| SC-010 | T003, T010–T017 | bounded manifests and retention classification for every failure |
| SC-011 | T002, T017 | owner/reuse justification for every production change; prior regressions pass; no parallel runtime |

## Story And Gate Coverage

| User story | Behavior tasks | Verification tasks | Independent acceptance boundary |
|---|---|---|---|
| US1 dynamic placed request | T004–T006 | T010–T012 one-role/four-role selectors | one complete REQUEST-to-RESPONSE with one-to-one plan |
| US2 NDN tensor dataflow | T007–T009 | T010–T012 pipeline/tensor/hybrid selectors | exact NDN edge set and frozen oracle |
| US3 local proof | T010–T013 | manifests U/I/M/C | same source/fixture/candidate identities across ordered gates |
| US4 Tiger qualification | T014–T016 | manifests T0–T3 | unchanged SIF, complete GPU results, no fallback |

## Update Rule

T002 replaces planning-level file names with verified symbol/entry/caller details and current-session execution status. Subsequent tasks update only their rows. A requirement may name multiple implementation tasks, but it has one final acceptance proof. Adding or removing a requirement, task, owner, or gate requires updating this matrix in the same documentation change.
