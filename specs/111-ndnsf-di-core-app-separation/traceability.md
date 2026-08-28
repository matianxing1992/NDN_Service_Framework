# Traceability: NDNSF-DI Core/APP Separation

## Requirements to tasks and evidence

| Requirement | Design/contract | Primary tasks | Acceptance evidence |
| --- | --- | --- | --- |
| FR-001, FR-003, FR-004 | Plan ownership matrix; `contracts/ownership-matrix.md` | T013-T017, T036, T042, T069, T107, T129, T182 | import graph and Core-only inventory |
| FR-002 | Plan Phase B; `data-model.md` CoreExecutionPlan | T031-T050, T068 | Core contract/native execution gates |
| FR-005 | Plan Phase C; `contracts/python-optimization-surface.md` | T072-T106 | ten-policy plus adapter replacement/default matrix |
| FR-006 | Placement and assignment contracts | T032-T033, T075, T077-T079, T101 | Core rejection/recovery evidence |
| FR-007, FR-008 | `contracts/provider-assignment-policy.md` | T072-T087, T105 | one-role/multi-role deterministic assignment evidence |
| FR-009 | Installation/model contracts | T076, T102-T103, T146, T153-T155 | missing-adapter and registry tests |
| FR-010, FR-011 | `contracts/assignment-context.md` | T110-T129 | 100-pair concurrency gate |
| FR-012 | Security section | T026, T028, T126, T185 | frozen security matrices |
| FR-013 | Phase A/B native reference | T019-T025, T043-T050, T068, T184 | characterization/native results |
| FR-014, FR-015 | Compatibility contract | T003-T008, T065-T066, T104, T128, T174-T175, T190-T193 | manifest and exit evidence |
| FR-016, FR-017 | Installation profiles contract | T158-T168 | isolated package/profile and static OCI-input inventories |
| FR-018 | Compatibility contract | T017-T018, T066-T067, T099, T145, T157, T163 | legacy import/CLI results |
| FR-019 | Migration/rollback table | T012, T030, T070, T108, T127, T181, T189 | phase rollback evidence |
| FR-020 | Candidate boundary | T001-T002, T009-T010, T179-T180 | historical digest equality/new identity |
| FR-021 | iTiger boundary | T166-T171, T179, T186, T200 plus Spec 110 T219-T232 | Spec 111 static handoff and delegated runtime gates |
| FR-022 | Documentation plan | T176-T178, T195 | parity report |
| FR-023 | Negative matrix | T014-T016, T032-T035, T075-T077, T110-T115, T146, T165 | negative test results |
| FR-024 | Validation strategy | T027, T029, T187-T189 | matched canary comparison |
| FR-025 | Constitution/security design | T011, T026, T126, T185, T198 | CodeGraph/security/audit evidence |
| FR-026 | `contracts/optimization-extension.md`; complete Python surface | T072, T078-T098, T146, T173 | ten-policy and independent adapter contract tests |
| FR-027 | Optimization suite/ownership contracts | T080-T083, T097, T100-T101, T112, T145, T148-T150 | instance/default/concurrency evidence |
| FR-028 | External package and loader contract | T076, T080, T083, T098, T105, T159, T164 | clean external wheel/allowlist evidence |
| FR-029 | Decision evidence data model | T072, T078-T083, T100-T101, T105 | digest/seed/budget/failure evidence |
| FR-030 | Core/provider authority matrix | T075, T077-T079, T085-T096, T101, T105, T149-T150 | malicious decision rejection matrix |
| FR-031 | Failure/fallback contract | T076, T078-T083, T101, T105 | timeout/exception/missing dependency/fallback evidence |
| FR-032 | External acceptance samples | T076, T098, T106, T146, T164, T173 | wheel, MiniNDN and native out-of-tree evidence |
| FR-033 | Installation artifact contract | T158-T168, T173, T186 | clean separate distribution/SBOM evidence |
| FR-034 | Invocation-site executor ownership | T082, T093, T100-T101, T112, T145, T148-T150 | deployment/client/provider executor evidence |
| FR-035 | Extension trust/containment contract | T076, T081-T083, T105, T164, T177-T178 | allowlist identity, worker containment and trust documentation |
| FR-036 | `contracts/python-optimization-surface.md` inventory closure | T084, T104-T105, T182, T196 | zero unclassified/native-only policy sites |
| FR-037 | Default-suite contract | T072, T084-T097, T104-T108 | frozen default parity and explicit default evidence |
| FR-038 | Python policy/SPI reachability contract | T076, T078-T082, T084-T098, T100, T105-T106, T146, T173 | all ten policies plus Runner adapter and optional observer externally consumed from Python |
| FR-039, FR-044 | Unified assignment and joint variant/partition/Provider/target contracts | T072-T075, T085-T088, T094, T096-T097, T100-T106 | one/multi-role and authorized variant-bound plan/target assignment evidence |
| FR-040 | Scheduling/dispatch contract | T078-T079, T089, T095, T101, T105-T106 | ordering plus concurrency/window atomic dispatch evidence |
| FR-041 | Cache/assignment boundary | T075, T090, T094, T101, T105-T106 | cache affinity and final assignment separation evidence |
| FR-042 | Recovery delegation contract | T075, T088, T092-T094, T100-T106 | directive-to-owner delegation evidence |
| FR-043 | Target/adapter/registry boundary | T076, T078-T083, T096-T098, T101-T106, T146 | explicit planner lookup and selected-adapter creation evidence |
| FR-045, FR-046 | `contracts/distributed-inference-engine.md`; engine composition and decision DAG | T072, T075, T078-T082, T084, T100-T101, T105-T106, T145, T148-T150 | graph edge/epoch/invalidation/re-entry, process-local ownership and evidence closure |
| FR-047, FR-048 | `data-model.md` objective/snapshot; engine contract | T072, T075, T078-T082, T100-T101, T105, T145 | exact-constraint precedence, least-input projection and stale/mixed snapshot rejection |
| FR-049 | `contracts/model-variant-policy.md` | T072, T075-T079, T097-T098, T100, T102-T106 | exact-model not-applicable, authorized alternative and quality/lineage evidence |
| FR-050 | Deployment lifecycle contract in optimization surface | T072, T075, T078-T079, T093, T100, T105-T106, T150 | use/activate/reserve/prewarm/scale/unload plus active lease/session rejection |
| FR-051 | Scheduling contract in optimization surface | T072, T075, T078-T079, T089, T100-T101, T105-T106, T149 | compatible batching, token phase, fairness, preemption/resume and hedge evidence |
| FR-052 | Typed tuning contract in optimization surface | T072, T075, T078-T079, T095, T100, T105-T106, T149 | declaration/type/range/scope and no-dispatch mutation matrix |
| FR-053 | Phase/action cache contract in optimization surface | T072, T075, T078-T079, T090, T094, T100-T101, T105-T106, T149 | pre/post epoch, closed action, atomic state and no-final-assignment evidence |
| FR-054 | Ownership anti-overfactoring map | T013-T014, T078-T079, T084, T104-T105, T176 | zero duplicate placement/resource/load/scaling/parallelism/communication/memory/backend authority |
| FR-055 | Revised pre-implementation gate | T030, T064, T084, T197-T198 | strict structure, distributed consistency fault gate, zero unresolved analysis CRITICAL/HIGH and code-aware PASS before default migration/large SDK implementation |
| FR-056, FR-057 | Metric-aware objective and `EstimateEnvelope` data model | T072, T075, T078-T082, T100, T105, T145 | unit/direction/aggregation/normalization and confidence/horizon/freshness matrix |
| FR-058, FR-059 | Scoped scheduling/admission contracts | T072, T075, T078-T079, T089, T091, T100, T105-T106, T148-T149 | request-DAG/provider-local and engine/provider scope isolation evidence |
| FR-060 | Adapter `BatchCapability` and role-target contract | T072, T075-T079, T089, T096, T098, T105-T106, T146, T149 | mixed/homogeneous batch and device/KV/checkpoint capability validation |
| FR-061 | `contracts/execution-intent-and-feedback.md`; distributed consistency contract | T051-T064, T072, T075, T078-T079, T100, T105, T145, T148-T149 | prepare/revalidate/commit/certify/activate/abort/release fault matrix with no executable torn state |
| FR-062 | Deployment lifecycle stabilization | T072, T075, T078-T079, T093, T105-T106, T150 | idempotency/readiness/cooldown/residency/drain and rollback evidence |
| FR-063 | Exact request semantics | T075, T078-T079, T095, T097, T102, T105 | tokenizer/prompt/sampling/stop/decoding mutation rejection |
| FR-064 | Candidate cardinality/work budget | T072, T075, T078-T082, T088, T097, T100, T105 | deterministic bounded pruning and lineage evidence |
| FR-065, FR-069 | Least-input and multi-tenant cache/privacy | T075, T078-T081, T090, T105, T149 | zero prompt/tensor/secret/cross-tenant disclosure and atomic cache binding |
| FR-066 | Core progress/checkpoint/output-commit recovery | T075, T078-T079, T092, T101, T105-T106, T149 | no duplicate/reordered/stale visible output under fault injection |
| FR-067, FR-068 | Optional outcome observer and state lineage | T072, T075-T076, T078, T080-T082, T098, T100, T105, T145, T148 | idempotent off-path outcome delivery, state replay and failure isolation |
| FR-070 | Ten policies plus two independent SPIs closure | T072, T076, T078-T084, T098, T104-T105, T196 | inventory shows exactly ten policy ports, Runner adapter and optional observer |
| FR-071 | `contracts/distributed-execution-consistency.md` authority model | T051, T054, T057, T060, T063 | one requester coordinator per attempt and one deployment writer per lifecycle stream |
| FR-072 | Existing lease/V2/Targeted mechanism reuse | T051-T052, T058-T061 | no second lease table, top-level service name or competing activation path |
| FR-073, FR-074 | Authenticated receipts and exact `ExecutionCommitCertificate` | T052-T053, T057-T061, T064 | exact complete receipt set required before Selection/activation |
| FR-075 | Request/Provider/deployment fencing tuple | T052-T057, T059-T063 | stale attempt, boot and lifecycle epochs rejected |
| FR-076 | Requester crash/restart and result rendezvous | T054, T057, T060-T062, T064 | same-identity recovery, higher-attempt fencing and one visible terminal result |
| FR-077 | Network-partition safety boundary | T052, T056, T060-T062, T064 | no new authority under partition and bounded in-doubt reservations |
| FR-078 | Periodic orphan cleanup | T051-T052, T056-T058, T062, T064 | idle-time reclamation of every declared resource type within bound |
| FR-079 | Deployment action certificate and CAS lifecycle | T055, T057, T063-T064 | idempotent replay, conflicting-writer rejection and destructive fail-closed action |
| FR-080 | Atomic execution/output visibility semantics | T053-T054, T057, T060-T062, T064 | no partial execution and no duplicate/reordered visible attempt output |
| FR-081 | Verifiable receipt evidence preservation | T052-T053, T057, T059-T060, T064 | signer/certificate/Data-name/wire-digest or equivalent proof retained |
| FR-082 | Blocking distributed fault matrix | T052-T056, T064, T182-T185, T197-T198 | all phase boundaries, crashes, partitions and fencing faults pass before Engine migration |
| FR-083 | `contracts/deployment-and-invocation-workflow.md` canonical terms and bounded aliases | T130, T135-T136, T140, T143 | one definition/revision/session/handle vocabulary and metadata-only `deploy_plan()` compatibility evidence |
| FR-084 | Immutable revision digest and idempotent reconciliation | T130, T132, T136, T138, T144 | canonical digest replay and same-revision no-op evidence |
| FR-085 | External artifact and secret boundary | T130, T136, T139, T143-T144, T166-T168, T224 | digest-bound external model mount with zero secret/weight journal or image inclusion and owner-injected envelope key material |
| FR-086 | Fixed identity-namespaced APP RuntimeJournal and protected request-envelope spool/reference | T131, T133, T137-T138, T140-T141, T144, T210, T218, T220, T224-T225 | owner/cross-tenant/traversal/locking/corruption/version/quota/restart, external key ownership, persistent-root policy, grouped durability, bounded compaction and envelope tamper/expiry/cleanup matrix |
| FR-087 | APPDeployment lifecycle API | T132, T138, T141-T144, T150, T156 | validate-to-delete API and CLI parity evidence |
| FR-088 | Revision-scoped readiness and activation | T134, T138-T139, T141, T144 | signed role/revision/boot/artifact/permission/capacity readiness closure |
| FR-089 | Ordered startup/bootstrap and process-provisioning boundary | T132, T134, T138-T139, T143-T144 | doctor/controller/NFD/generic-provider-agent/apply/client startup evidence |
| FR-090 | Durable request handle and compatibility adapters | T133, T136, T140-T141, T143-T144, T148, T210, T219, T221-T222 | requester/attempt/revision/certificate/rendezvous-bound reopen/status/result/stream across APP restart, dynamic-plan request-ID propagation and sync/Future parity |
| FR-091 | Cancellation semantics | T133, T140-T141, T144, T222-T223 | pre-certificate local abort plus authenticated post-certificate bounded Provider cancellation, replay/stale fencing and accepted-terminal preservation |
| FR-092 | Reconciliation, upgrade and rollback epochs | T132, T134, T138-T141, T144 | partial apply recovery, new-revision upgrade and rollback-as-new-epoch evidence |
| FR-093 | Graceful drain and shutdown | T134, T138-T141, T144, T149 | admission closure, deadline-bounded completion/cancel and resource release evidence |
| FR-094 | Thin operations CLI parity | T135, T142, T156-T157 | CLI invokes APP semantics with matching status/errors and no second lifecycle implementation |
| FR-095 | Revision binding in request/receipt/result evidence | T133-T134, T139-T141, T144 | mismatched revision rejection and certified-result lineage |
| FR-096 | Blocking clean-profile MiniNDN operational gate | T143-T144, T182-T185, T197-T198 | validate-to-delete workflow including restart, upgrade/rollback and drain |
| FR-097 | `contracts/itiger-slurm-apptainer-handoff.md` OCI/SIF runtime boundary | T171, T186, T200 | offline Docker-daemon/public-service negative scan and Spec 110 handoff |
| FR-098 | Distinct infrastructure/deployment/request handles | T136, T141, T171, T186, T200 | state-schema separation and handoff mutation evidence |
| FR-099 | Revision-derived same-SIF process-map boundary | T136, T171, T186, T200 | arbitrary-role and host-executable negative fixtures delegated to Spec 110 |
| FR-100 | iTiger least-privilege state/model/identity/scratch binds | T131, T137, T166-T171, T186, T200 | container bind inventory and cross-role/broad-project negatives |
| FR-101 | Shared node-run and per-Provider GPU binding | T139, T168, T171, T186, T200 | NFD socket/GPU UUID handoff validation |
| FR-102 | Infrastructure-to-APP lifecycle order | T132, T134, T138-T144, T171, T186, T200 | offline readiness-order fixture and MiniNDN APP lifecycle gate |
| FR-103 | Selected-transport multi-node boundary | T171, T186, T200 | no-live/no-persistent-route handoff and Spec 110 selected-transport dependency |
| FR-104 | New post-Spec-111 candidate identity | T179-T180, T186, T200, T212, T215, T220 | two immutable rejected candidates remain preserved; T220 deliberately spends zero additional candidate because its diagnostic predicts rejection, so T200 remains blocked |
| FR-105 | MiniNDN-only distributed acceptance and deferred container/iTiger execution | T027, T029, T064, T106, T144, T170-T171, T185-T187, T194, T200 | local/static gate plus MiniNDN evidence with zero OCI/SIF build, container-runtime invocation or Slurm job |

## Success criteria closure

| Criterion | Closing tasks | Evidence |
| --- | --- | --- |
| SC-001 | T014, T069, T107, T129, T182 | zero forbidden imports |
| SC-002 | T015-T016, T069, T164-T165 | Core-only module and static OCI-profile inventory |
| SC-003 | T003-T008, T174, T191 | 100% manifest coverage |
| SC-004 | T072-T086, T105 | policy decisions and Core rejection matrix |
| SC-005 | T110-T125 | 100 overlapping paired requests, zero bleed/env mutation |
| SC-006 | T182-T186, T194 | complete regression gates |
| SC-007 | T027, T029, T187-T189, T212, T214-T215, T217, T220, T225, T213 | two immutable rejected ten-pair campaigns, focused remediation diagnostics and unchanged final 5% acceptance gate; T225 may spend the only next candidate after its prediction gate, and T213 requires acceptance |
| SC-008 | T065-T066, T174-T175, T190-T193 | compatibility usage and two-snapshot exit gate |
| SC-009 | T001-T002, T009-T010, T179-T180 | immutable history and new candidate identity |
| SC-010 | T176-T178, T195 | English/Chinese parity evidence |
| SC-011 | T076, T080-T083, T098, T105, T159, T164 | standalone wheel and contract-test evidence |
| SC-012 | T075, T077-T079, T105-T106 | external MiniNDN smoke and rejection matrix |
| SC-013 | T112-T113, T125 | 100 two-suite concurrent invocations, zero bleed |
| SC-014 | T146, T158-T168, T173, T186 | separate artifacts and out-of-tree native build |
| SC-015 | T014-T016, T078, T158-T165, T182, T186 | zero Core-to-SDK imports and disjoint wheel ownership |
| SC-016 | T084, T105, T182, T196 | complete inventory, ten-policy mutation and selected-adapter evidence |
| SC-017 | T072, T097, T105, T108 | default parity and partial-suite named-default evidence |
| SC-018 | T072, T075, T078-T082, T084, T100-T101, T105, T145 | complete engine graph and undeclared/stale/cyclic transition rejection |
| SC-019 | T072, T075, T078-T082, T100-T101, T105 | objective mutation, hard-constraint precedence and snapshot epoch evidence |
| SC-020 | T072, T075-T079, T097-T098, T100, T102-T106 | three-size/two-profile model variant, exact constraint and lineage matrix |
| SC-021 | T072, T075, T078-T079, T093, T105-T106, T150 | complete deployment lifecycle action and lease/session rejection matrix |
| SC-022 | T072, T075, T078-T079, T089, T105-T106, T149 | dynamic batching, phase/fairness/preemption/hedge and rejection matrix |
| SC-023 | T072, T075, T078-T079, T095, T105-T106, T149 | typed tuning valid-boundary and invalid-result matrix |
| SC-024 | T072, T075, T078-T079, T090, T094, T105-T106, T149 | cache action/phase/atomicity and assignment-boundary matrix |
| SC-025 | T072, T075-T079, T088, T094, T096-T097, T100, T105 | joint variant/plan/Provider/target feasibility and torn-tuple rejection |
| SC-026 | T072, T075, T078-T082, T088, T097, T100, T105 | metric/estimate rejection and seeded candidate-pruning replay |
| SC-027 | T072, T075, T078-T079, T089, T091, T096, T105-T106, T149 | both admission/scheduling scopes and adapter batch capabilities |
| SC-028 | T075, T078-T079, T100, T105, T145, T148-T149 | atomic execution-intent fault matrix and resource release |
| SC-029 | T072, T075, T078-T079, T093, T105-T106, T150 | lifecycle idempotency/cooldown/readiness/drain matrix |
| SC-030 | T075, T078-T079, T092, T101, T105-T106, T149 | streaming checkpoint recovery with zero duplicate visible output |
| SC-031 | T072, T075-T076, T078, T080-T082, T098, T100, T105, T145, T148 | observer logical exactly-once, state lineage and failure isolation |
| SC-032 | T075, T078-T081, T090, T105, T149 | least-input and cross-tenant nondisclosure matrix |
| SC-033 | T052-T053, T060-T061, T064 | every phase-boundary fault yields a complete certified attempt or no executable attempt |
| SC-034 | T052-T053, T059-T060, T064 | duplicated/reordered/lost operations remain idempotent or fail closed with exact receipt closure |
| SC-035 | T054, T060-T062, T064 | requester crash/restart produces at most one visible terminal attempt and bounded cleanup |
| SC-036 | T055, T063-T064 | competing lifecycle writers produce one valid action stream and no partial destructive transition |
| SC-037 | T052, T056, T060-T062, T064 | partition permits no new authority and expires unreachable reservations within bound |
| SC-038 | T052-T053, T056-T062, T064 | Provider restart fences old boot epoch, lease, receipt and activation authority |
| SC-039 | T056-T058, T062, T064 | idle periodic sweep reclaims every declared orphan resource within bound |
| SC-040 | T064, T182-T185 | MiniNDN consistency smoke and existing lease/security regressions pass together |
| SC-041 | T130, T136, T138 | identical resolved inputs yield one digest; changed execution input yields a new digest |
| SC-042 | T131-T132, T137-T138, T144 | apply replay and APP restart recover one operation without duplicate resource action |
| SC-043 | T134, T138-T139, T141, T144 | READY/ACTIVE requires complete fresh revision-scoped role evidence |
| SC-044 | T131, T133, T137, T140-T141, T144, T210, T219, T221-T223 | accepted request reopens from a requester/attempt-fenced protected digest-matching envelope and authenticated result rendezvous with one visible terminal result; durable identity crosses both predeployed/dynamic runtime paths; stale cancellation and missing/tampered input fail closed |
| SC-045 | T131, T137, T144, T218, T220, T224-T225 | journal permissions/corruption/outer-and-nested-version/locking/quota/compaction, persistent-root enforcement and external protected-spool key/durability faults fail closed and preserve diagnostics |
| SC-046 | T132, T134, T138-T141, T144 | upgrade and rollback create fenced revision/lifecycle epochs with no mixed-revision result |
| SC-047 | T134, T139-T141, T144 | drain accepts zero new work and reaches INACTIVE within its declared deadline policy |
| SC-048 | T135, T142, T156-T157 | operations CLI and Python API expose identical lifecycle semantics and stable machine output |
| SC-049 | T143-T144 | clean MiniNDN path completes validate, resolve, plan, apply, ready, active, submit, reopen, result, drain and delete |
| SC-050 | T135, T140, T148 | `deploy_plan()` remains metadata-only and emits a deprecation signal toward `prepare_session()` |
| SC-051 | T171, T179-T180, T186, T200 | handoff rejects mutable/pre-Spec-111/wrong revision-SIF-model-state identity |
| SC-052 | T141, T171, T186, T200 | scheduler/APP/request states remain mechanically distinct |
| SC-053 | T131, T137, T166-T171, T186 | least-privilege bind and host-executable negative inventory |
| SC-054 | T136, T139, T168, T171, T186 | revision-derived same-SIF roles and GPU mapping contract fixtures |
| SC-055 | T132-T144, T171, T186 | preflight-to-drain readiness order with no partial advance |
| SC-056 | T179-T180, T200 | new-candidate live acceptance is delegated, not claimed, through the Spec 110 handoff |
| SC-057 | T171, T186, T200 | multi-node render requires selected-transport digest and one-NFD-per-node contract |
| SC-058 | T165-T168, T171, T186, T200 | zero Docker daemon/model-in-image/always-on-service claim |
| SC-059 | T027, T029, T064, T106, T144, T170-T171, T185-T187, T194, T200 | MiniNDN-only distributed evidence and zero new OCI/SIF/registry/Apptainer/Slurm/iTiger result |

## User story closure

| Story | Independent gate | Tasks |
| --- | --- | --- |
| US1 | Core-only import + native deterministic execution + certificate-gated distributed fault matrix | T031-T071 |
| US2 | Standalone optimizer wheel + process-local engine + ten policies + Runner/observer SPIs + joint atomic decisions + named defaults + Core rejection matrix | T072-T109 |
| US3 | 100 paired concurrent assignments, zero bleed/global mutation | T110-T129 |
| US4 | Revision-bound deploy/use lifecycle + durable invocation + isolated profiles + migrated callers + immutable evidence + rollback | T130-T181 |

## Governance and closure task mapping

| Task | Governing contract | Closing evidence |
| --- | --- | --- |
| T196 | Complete FR/SC/contract/task reconciliation | `traceability.md` structural coverage check: 105/105 FR, 59/59 SC, now reconciled through all 220 tasks |
| T197 | Cross-artifact consistency gate required by the repository Spec Kit workflow | `evidence/speckit-analysis.md` |
| T198 | Strict code-aware post-implementation audit | `AUDIT.md` |
| T199 | Resumable-state and unrelated-worktree exclusion required by the repository GSD workflow | `evidence/gsd-final-health.md` |
| T200 | Spec 110 ownership and candidate handoff | `../110-itiger-qwen-live-inference/handoffs/spec111-separation.md` |
| T201 | Repository completion bell, final status and next-step handoff rule | `completion-summary.md` |

## Remediation task closure

| Tasks | Trigger | Evidence |
| --- | --- | --- |
| T202-T203 | Initial formal matrix exposed real role-import/startup ownership failures | `evidence/completion-hold.md`, `evidence/role-entrypoint-remediation.md` |
| T204-T205 | Candidate regeneration and real Controller/Provider/User readiness were required before performance work | `evidence/post-separation-candidate.json`, final campaign readiness records |
| T206-T207 | Old failure attribution and all historical negative outcomes had to remain immutable | `evidence/non-regression-campaign.md`, `evidence/performance-comparison.md`, `evidence/performance-gate.md` |
| T208-T209 | Terminal-cell diagnostic continuation must skip completed/failed cells and can never become formal evidence | campaign continuation marker and resume-semantics tests |
| T210-T211 | Canonical APP durability and authenticated Provider lifecycle evidence had to replace volatile/local-lambda authority | request-handle and real workflow gates |
| T212, T214-T215 | Two single-candidate matrices failed SC-007 and remediation could not overwrite either result | `evidence/t212-negative-result-and-remediation.md`, `evidence/t215-negative-result-and-durability-audit.md` |
| T216-T217 | Root-cause and strict acceptance audit prohibited post-hoc weakening of SC-007 | `AUDIT.md`, `evidence/t215-negative-result-and-durability-audit.md` |
| T218-T219 | Full RuntimeJournal and InferenceRequestHandle contracts were required after the durability audit | `evidence/t218-t219-durable-runtime-gate.md` |
| T220 | Correct durable optimization and one diagnostic predicted another SC-007 rejection, so zero new candidate/matrix was generated | `evidence/t220-durable-performance-convergence.md` |
| T221 | Dynamic-plan facade dropped the canonical durable request ID | `evidence/t221-dynamic-request-identity.md` |
| T222-T223 | APP cancellation currently stops only a process-local Future and must bind the existing Provider execution-control payload to certified membership | pending code/unit/MiniNDN cancellation evidence |
| T224 | Production persistence and envelope-key ownership remain weaker than the operator-mounted/external-secret contract | PASS: owner-injected static/file keyring, no journal-owned key bytes, v3 key identity/rotation, explicit persistent roots, volatile-root rejection and named MiniNDN-only override; `evidence/t224-persistent-root-and-envelope-key.md` |
| T225 | A grouped durability design and predictive diagnostic must precede the only next candidate/matrix | pending local/diagnostic/formal evidence; T212/T215 remain immutable |
| T213 | Final strict audit remains open until a future accepted candidate exists | `AUDIT.md` |

## Evidence levels

- Tasks and code before execution are `proposed`.
- Moved code with focused tests is `implemented` or `executed` only as recorded.
- MiniNDN results become `measured` only after T029/T187 produce admissible artifacts.
- No Spec 107/109/110 result is evidence for the Spec 111 candidate.
