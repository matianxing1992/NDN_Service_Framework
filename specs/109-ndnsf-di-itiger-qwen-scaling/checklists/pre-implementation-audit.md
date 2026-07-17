# Pre-implementation Audit: Spec 109

## Status

`PASS — Revision 2 resolves the six HIGH and four MEDIUM design/contract findings. Offline implementation may begin at T001; every live action remains gated by its exact source, predecessor, deployment, storage, and semantic-validation prerequisites.`

No Spec 109 model transfer, export, or Slurm inference job has been executed. This file is the design-readiness audit only; T163 must create `post-implementation-audit.md` from real code, tests, jobs, and evidence.

## Audit findings and applied document changes

| Finding | Severity | Remediation |
|---|---|---|
| Transformers/full-model timing was an invalid overhead baseline | HIGH | Three planes: full-model correctness oracle, matched staged ONNX baseline, NDNSF-DI candidate; overhead is candidate minus staged baseline only |
| Model size was confounded with GPU/placement and workload/statistics were underspecified | HIGH | Descriptive full ladder plus controlled common-hardware subset; locked load/cache/run order; counts, CIs, and percentile thresholds |
| CUDA provider registration could falsely imply GPU execution | HIGH | Complete ORT node-to-provider profiling and allocation-correlated GPU UUID required for GPU PASS |
| JSON contracts accepted duplicate/contradictory/false-authority records and copied Spec 108 resources | HIGH | Keyed cells/runs, conditional Schemas, source/predecessor Schemas, Spec 108 digest composition, canonical semantic validator |
| Spec 107 predecessor gate used an undefined task range | HIGH | Exact manifest for Spec 107 T027/T028-T038 and Spec 108 T091-T102 with status/schema/path/digest |
| Dirty/untracked source was not reconstructable | HIGH | Clean or sealed-dirty SourceSnapshot with binary diff and untracked manifest/archive digests |
| Exact tokens alone missed numerical divergence | MEDIUM | Hidden-state, KV, logit, top-1, and margin tolerances plus exact token arrays |
| One local tier failure censored later models | MEDIUM | Systemic/model-local/placement-local gates and explicit dependency propagation |
| Bundled ladder tasks hid partial cells | MEDIUM | Immutable keyed ledger; per-cell terminal closure inside bundled jobs |
| Personal Skill paths and audit reuse were nonportable/ambiguous | MEDIUM | Repository-local CLI/scripts canonical; Skill optional; distinct pre/post audits |

## Required validation before verdict

- [x] Strict Spec Kit structure passes with all 54 FRs and 22 SCs traced.
- [x] All five JSON Schemas parse; a valid cross-file profile resolves and is accepted.
- [x] Terminal contradiction, unsealed dirty source, incomplete workload/predecessor, false authority, CPU-node GPU PASS, invalid p99, and failed numerical-checkpoint probes are rejected.
- [x] T001-T165 remain unique/sequential and every bundled live task requires keyed per-cell terminal closure.
- [x] `git diff --check` passes for Spec 109.
- [x] CodeGraph confirms the reusable `run_matched_staged_baseline` path and that current `OnnxRuntimeModelRunner` evidence records selected provider but not complete node assignment; both are explicitly tasked.
- [x] GSD health is `healthy`, CodeGraph is up to date, and the agent-context hook selects Spec 109.

## Validation result

```text
Structural verdict: PASS
functional_requirements: 54
success_criteria: 22
user_stories: 6
tasks: 165 (0 complete; IDs unique and sequential)
traced_requirements: 54

valid_profile: ACCEPTED
incomplete_workload: REJECTED
missing_predecessor: REJECTED
terminal_contradiction: REJECTED
unsealed_dirty: REJECTED
contradictory_authority: REJECTED
cpu_model_node: REJECTED
invalid_p99: REJECTED
failed_checkpoint: REJECTED
```

This PASS grades document readiness, not implementation or experiment success. The canonical semantic validator, ORT node profiling, matched baseline runner, and all live evidence still have unchecked tasks.

## T010 execution record

Executed on 2026-07-13 against source snapshot
`sha256:8dce660b3f8d568952650006fc026b9f5449b41add1cd9beea94a5960fc946cc`:

```text
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks

Structural verdict: PASS
functional_requirements: 54
success_criteria: 22
user_stories: 6
tasks: 165
tasks_complete: 0
parallel_tasks: 36
story_task_counts: {'US1': 20, 'US2': 20, 'US3': 30, 'US4': 20, 'US5': 15, 'US6': 15}
has_traceability: True
traced_requirements: 54
```

`git diff --check` also exited 0. This record predates checking T001-T010 and
therefore correctly reports `tasks_complete: 0` for the scanned input.

## Execution boundary

After a successful revised verdict, only offline T001-T035 may start. Live discovery begins at T047. Transfer begins at T050 only after admission. Candidate work remains blocked until T089 proves the exact predecessor/source/deployment manifests.
