# Spec 168 traceability and bounded claims

This is the post-run reconciliation for the current dirty worktree and the
immutable Spec 168 evidence. `PASS` means the requirement has implementation
and exercised evidence; it does not promote an environmental failure or an
unrun experiment to a success.

## Functional requirements

| Requirement | Owner / task | Validation and evidence | Status |
|---|---|---|---|
| FR-001–FR-005 request identity, ACK closure, capability, deferred plan, feasibility | NDNSF-DI app/core; T003, T005, T006, T010 | lifecycle gate, deferred-planning, per-role-dataflow, small control Job 182518 | PASS for accepted small/control paths |
| FR-006–FR-008 immutable fetch, residency, reuse-aware strategy | artifact deployment/provider/planner; T007, T009, T010, T011 | provider-generation, cache-residency, reuse-strategy, 30-row reanalysis | PASS for small schedule; large fetch is retained as failure |
| FR-009–FR-011 per-role execution, one invocation, CUDA fail-closed | execution/provider/adapter; T006, T007 | per-role-dataflow, provider-generation, Job 182518; no CPU fallback | PASS for accepted small path |
| FR-012–FR-014 terminal state, deadlines, failure boundary/checkpoint | core state/status/deadline; T003, T012 | lifecycle-evidence and failure-taxonomy gates; OOM record for Job 182780 | PASS |
| FR-015–FR-018 real forwarding, security, immutable bindings, MiniNDN/exact SIF | gates and launchers; T004, T008, T014 | local gates, exact-SIF/CUDA preflight, Job 182518 and v93 admission manifests | PASS for gates and small run; large response absent |
| FR-019–FR-022 immutable failures, small-before-large, cold/warm rows, defect lineage | campaign/evidence; T001, T002, T011, T013 | retained 182508/509/773/778/780/782 negatives, 182777 reanalysis, `evidence/defects/closure-matrix.md` | PASS for the bounded campaign rules; T015/T016 remain downstream gaps |
| FR-023–FR-024 NDNSF-DI ownership and evidence category separation | plan/contracts/analyzers; T003, T007, T012, T017 | plan ownership model, failure taxonomy, small/large evidence separation | PASS, with residual large-model gap |
| FR-025–FR-027 bounded Selection fanout, mapped native ABI, pre-model canary | native/runtime launch; T004, T006, T013 | focused regressions, Gate C, canary evidence, control-plane defect records | PASS for admitted control/small candidate; no large inference claim |

## Success criteria

| Criterion | Evidence | Status |
|---|---|---|
| SC-001–SC-003 complete small request, one lineage, per-role starts | `evidence/tiger-small-single/182518-v88-qwen3-small-single`, provider-generation, per-role-dataflow | PASS |
| SC-004–SC-005 complete repeated schedule and causal reuse | `evidence/tiger-small-repeated/182777-v92-fetch-event-boundary-requalified/analysis.json`, `small-cold-warm-analysis.md` | PASS for one immutable allocation; descriptive only |
| SC-006–SC-007 classified failures and repair regressions | failure-taxonomy gate and `evidence/defects/closure-matrix.md` | PASS for the encountered logic defects; environmental/runtime gaps remain explicit |
| SC-008 large model completes three-node response | Jobs 182778/182780/182782 evidence; v94 reached three CUDA `RUNTIME_READY` markers and returned one authenticated response, but exact reference acceptance failed (`TOKEN_MISMATCH`, 64 tokens) | NOT MET; deterministic acceptance failed and no retry admitted |
| SC-009 clean-allocation reproduction | `evidence/reproducibility.md`, `evidence/tiger-clean-reproduction/README.md` | NOT RUN; blocked by SC-008 |
| SC-010 security verdict | small control/repeated analyzer security verdicts and local security regressions | PASS for accepted small evidence |
| SC-011 final audit and bounded mapping | this file and `evidence/final-audit.md` | PASS once this reconciliation is retained |
| SC-012 >7 KiB provider-specific Selection projections | selection transport regressions and canary/defect evidence | PASS for control-plane path |

## Machine-readable requirement index

The grouped tables above are expanded here so automated traceability checks can
bind every identifier to this audit and to the same evidence boundaries:

`FR-001` `FR-002` `FR-003` `FR-004` `FR-005` `FR-006` `FR-007` `FR-008`
`FR-009` `FR-010` `FR-011` `FR-012` `FR-013` `FR-014` `FR-015` `FR-016`
`FR-017` `FR-018` `FR-019` `FR-020` `FR-021` `FR-022` `FR-023` `FR-024`
`FR-025` `FR-026` `FR-027`

`SC-001` `SC-002` `SC-003` `SC-004` `SC-005` `SC-006` `SC-007` `SC-008`
`SC-009` `SC-010` `SC-011` `SC-012`

## Evidence boundaries

The current evidence supports deployment-faithful NDNSF-DI operation for the
pinned small model, including secured multi-provider lifecycle, complete
multi-token response, and one cold/warm schedule. It does not support a
large-model response, cross-allocation reproducibility, inferential performance
claims, or a claim that TigerCluster itself improves mobility. Job 182780's
18.53-GB verified fetch and v94's three CUDA runtime-ready markers are
Repository/preparation evidence only; v93's OOM and v94's response-level rank
failure remain negative outcomes.
