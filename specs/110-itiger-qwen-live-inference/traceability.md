# Traceability: NDNSF-DI iTiger Distributed Qwen Execution

## User intent to acceptance

| Source intent | Stories | Requirements | Primary tasks | Success/evidence |
|---|---|---|---|---|
| Use NDNSF-DI, not standalone Qwen | US2-US5 | FR-009-FR-026 | T065-T123 | SC-003-SC-009 |
| Install the complete environment on iTiger | US1 | FR-001-FR-006 | T031-T049 | SC-001 |
| Build OCI rootlessly in Slurm scratch and materialize Apptainer/SIF | US1 | FR-002-FR-006 | T031-T049 | runtime release verdict |
| Prove a controlled single-node three-provider/three-GPU candidate first | US2 | FR-009-FR-011, FR-018-FR-021 | T065-T084 | SC-003, SC-008 |
| Add multi-node NDN as an independently keyed extension | US3 | FR-007-FR-010 | T050-T064, T088-T089 | SC-002, SC-004 |
| Test every requested Qwen size under the controlled placement | US4 | FR-012-FR-017, FR-033 | T085-T105 | SC-005 |
| Measure speed and overhead with matched contrasts | US5 | FR-022-FR-026 | T106-T123 | SC-006-SC-009 |
| Preserve storage, failures, operations, and authority evidence | US6 | FR-027-FR-036 | T124-T147 | SC-010-SC-013 |

## Functional requirement coverage

| Requirement | Design/contract | Implementation tasks | Test/evidence tasks |
|---|---|---|---|
| FR-001 | plan: allocation lifecycle | T022, T040, T053 | T047-T048, T061-T062, T127-T128 |
| FR-002 | `contracts/runtime-release.md` | T031-T044 | T042, T046-T049 |
| FR-003 | runtime release host/runtime split | T031-T041 | T037, T048 |
| FR-004 | plan: storage layout | T018-T020, T039-T040, T044 | T045-T049, T130-T132 |
| FR-005 | runtime forbidden content | T028, T035, T040, T057 | T042, T130 |
| FR-006 | LiveClusterSnapshot | T008-T009, T022, T044 | T045-T049, T098, T101 |
| FR-007 | selected-transport multi-node gate | T050-T056, T063 | T060-T064, T088-T089 |
| FR-008 | `contracts/allocation-topology.md` | T051, T053-T054 | T055, T062, T127 |
| FR-009 | security architecture | T057-T058, T067, T069 | T059, T062, T073, T079-T083 |
| FR-010 | placement classes and process map | T013-T015, T050-T055, T077 | T079-T084, T088-T105 |
| FR-011 | GenerationSessionRecord | T065-T072 | T073, T079-T083 |
| FR-012 | ModelArtifactSet | T004, T085-T087 | T090-T105 |
| FR-013 | DistributedCandidate | T006-T010, T077, T085 | T007, T087-T105 |
| FR-014 | erratum and identity contract | T006-T007 | T105, T143 |
| FR-015 | execution-state contract | T011-T017 | T078-T105 |
| FR-016 | execution-state contract | T011-T017 | T012, T027, every live task |
| FR-017 | execution-state contract | T011-T017 | T012, T079-T105 |
| FR-018 | GPU execution proof | T026, T036, T070 | T027, T048, T079-T105 |
| FR-019 | run-evidence schema | T013-T015, T036-T037 | T048, T079-T105 |
| FR-020 | workload/correctness | T005, T024-T025, T066 | T075-T076, T079-T105 |
| FR-021 | 0.5B gate | T074-T077 | T078-T084 |
| FR-022 | performance protocol | T024-T025, T106-T108 | T109-T123 |
| FR-023 | research Decision 7 | T107-T108 | T109-T120 |
| FR-024 | PerformanceRecord | T106-T108 | T109-T123 |
| FR-025 | workload thresholds | T024-T025, T106 | T108, T118 |
| FR-026 | variables/confounders | T107-T108 | T118-T120 |
| FR-027 | operator CLI and crash-safe journal | T016-T017, T022 | every submit task, T125-T126 |
| FR-028 | replacement identity | T006-T007, T017 | T121-T122 |
| FR-029 | EvidenceBundle/schema | T013-T015, T026 | T079-T132 |
| FR-030 | storage/promotion | T019-T020 | T129, T132 |
| FR-031 | cleanup policy | T018-T020 | T131 |
| FR-032 | authority dimensions | T014-T015 | T132-T133 |
| FR-033 | matrix completion | T011-T017 | T105, T132, T139, T146 |
| FR-034 | ownership table | T002-T003, T031-T073 | T073, T134-T142 |
| FR-035 | authority boundary | T014-T015 | T133, T144, T146 |
| FR-036 | operator CLI safety | T021-T023, T044 | T045-T048, T060-T061, T125, T130 |

## Success criteria evidence

| Criterion | Closing task | Canonical evidence |
|---|---|---|
| SC-001 | T048-T049 | `results/spec110-itiger-qwen-live/runtime-probe/manifest.json` |
| SC-002 | T060-T064 | `results/spec110-itiger-qwen-live/network-probe/manifest.json` |
| SC-003 | T078-T084 | `results/spec110-itiger-qwen-live/0.5b/single-node-correctness-*/manifest.json` |
| SC-004 | T088-T089 | `results/spec110-itiger-qwen-live/0.5b/multi-node-correctness-32/manifest.json` |
| SC-005 | T090-T105 | `evidence/size-ladder-verdict.md` plus seven size-local bundles |
| SC-006 | T109-T118 | per-size `performance/` bundles and `scaling/per-size.json` |
| SC-007 | T107-T120 | local-staged/single-node and single-node/multi-node matched contrasts |
| SC-008 | T026-T027, T079-T105 | stage/provider/GPU execution proofs |
| SC-009 | T121-T123 | `results/spec110-itiger-qwen-live/reproduction/` |
| SC-010 | T013-T015, T129-T133 | promotion, secret scan, validation, and release gate |
| SC-011 | T018-T020, T131 | storage admission and protected cleanup bundle |
| SC-012 | T053, T127-T128 | allocation teardown/process audit |
| SC-013 | T133, T139-T147 | release gate, traceability, handoff, and completion summary |

## Authority progression

```text
runtime probe PASS
  -> substrate only
single-node 0.5B three-provider/three-GPU PASS/FAIL
  -> first distributed candidate experiment evidence
selected-transport network probe PASS + single-node 0.5B PASS
  -> multi-node extension eligible
multi-node 0.5B PASS/FAIL
  -> placement/network experiment evidence
seven-size single-node matrix + matched performance
  -> Spec 110 experimental authority
Spec 110 completion
  -> physicalProduction remains DEFERRED to Spec 106
```

## Known non-evidence

- Spec 109 task checkboxes and BLOCKED cells;
- jobs 146050/146123, which transferred/sealed but did not infer;
- fixture-only Slurm, GRES, or network tests;
- `nvidia-smi` without model backend correlation;
- full-model or local-staged output without NDNSF-DI;
- a single-node result mislabeled as cross-node, or a network probe without Qwen stages;
- a post-`sbatch` receipt with no pre-submit intent journal.
