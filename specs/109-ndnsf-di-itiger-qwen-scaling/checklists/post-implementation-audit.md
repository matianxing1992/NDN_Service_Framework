# Spec 109 post-implementation audit

## Verdict

`CONDITIONAL PASS — Spec 109 is ready for honest terminal task closure, not for
GPU-performance or production acceptance.`

No unresolved CRITICAL or HIGH implementation finding remains. The source,
storage, model, candidate, matrix, evidence, and authority contracts are
implemented and tested; all 105 preregistered cells have a terminal record.
The mechanical release gate correctly remains `BLOCKED`: all 24 exact Spec
107/108 predecessors are incomplete, the unchanged MiniNDN preflight ended in
`local deadline`, no GPU inference job ran, 72B exceeds the provisional
project quota plus reserve, and multi-node remains gated by Spec 108 T134.

This verdict approves the implementation and negative-result closure. It does
not upgrade standalone, artifact, candidate, scaling, reproduction, or physical
authority.

## Findings

| ID | Severity | Dimension | Location | Finding | Resolution / remaining action |
|---|---|---|---|---|---|
| A1 | HIGH, resolved | Code reality / evidence | `OnnxRuntimeModelRunner.cpp`, `ProviderRoleWorker.cpp`, `NativeProviderHandler.cpp`, `DI_NativeProviderExecutable.cpp` | Initial runner evidence was copied before the first ORT run, so node-to-provider assignments produced by `EndProfiling` could not reach emitted provider evidence. | Added post-run evidence snapshots to `ProviderRoleResult`, handler observers for both local-full-plan and single-role paths, aggregate readiness-state updates, and `NDNSF_DI_EXECUTION_EVIDENCE_UPDATE`. Focused and full C++ tests pass. |
| A2 | HIGH, resolved | Operations / executability | `run-ndnsf-qwen.sh`, `DI_NativeProviderExecutable.cpp` | Allocation orchestration accepted any matching provider log and waited for a ready marker the native Provider never emitted; a real job could start early or time out forever. | The Provider now emits the marker only after permission/init/provisioning. The runner verifies the live Controller and every expected Provider PID/log before starting the User. A delayed two-Provider executable integration test proves the ordering. |
| A3 | HIGH, resolved | Concurrency / evidence integrity | `OnnxRuntimeModelRunner.cpp` | Concurrent first requests could race `EndProfiling`, `profilingCaptured`, and mutable execution evidence. | Added an atomic one-shot state, first-profile mutex, and copy-returning thread-safe evidence snapshot. Normal later inference remains concurrent. |
| A4 | MEDIUM, resolved | Security evidence | `scan_spec109_evidence.py`, `test_redaction.py` | A sealed untracked tar was read as one raw text file, creating false findings from the scanner's deliberate synthetic secret fixture and providing no member attribution. | The scanner now reads tar members without extraction, attributes each finding to `archive!member`, bounds member size, and skips only the exact registered synthetic fixture. Final scan: 236 text members/files, 1 archive, 1 synthetic fixture skipped, 0 findings. |
| E1 | MEDIUM, external/terminal | Validation evidence | `evidence/preflight-0.5b.md`, `release-gate.json` | The real MiniNDN request path still ends in `local deadline`, and no iTiger Qwen GPU candidate has executed. | Preserve as negative evidence. Owner sequence: resolve Spec 107 MiniNDN gate, close Spec 108 GPU release, then create a new source/candidate identity before any Spec 109 GPU submission. |
| E2 | LOW, evidence limit | Test environment | `results/spec109-itiger-qwen/final-tests/cpp-all.log` | Seven real-model/generated-plan C++ test bodies return early because external ONNX model or generated-plan environment variables are absent. | They are not counted as real-model execution. Their behavior remains covered by offline fixtures; live GPU authority stays blocked. |

## Spec Kit analyze report

The read-only cross-artifact pass found no unresolved duplication,
contradiction, ambiguity, placeholder, constitution conflict, uncovered
requirement, or unjustified task.

| Metric | Result |
|---|---:|
| User stories | 6 |
| Functional requirements | 54 |
| Success criteria | 22 |
| Tasks | 165, unique and sequential |
| Requirement/criterion coverage | 76/76 (100%) |
| Unmapped tasks | 0 |
| Placeholders | 0 |
| Critical / unresolved High | 0 / 0 |

`traceability.md` maps every FR and SC and accounts for every task range.
Outcome words such as `BLOCKED`, `FAIL`, and `INCONCLUSIVE` are evidence
outcomes, not unchecked implementation work. The task completion rule permits
conditional work to close only with explicit terminal records; the 105-cell
ledger has no `SUBMITTED` or `RUNNING` cell.

## Code and security reality

- CodeGraph was synchronized after implementation and used to trace ORT runner
  evidence through the worker, handler, provider executable, and tests.
- GPU PASS validation requires complete model-node assignments, CUDA for every
  model node, no undeclared fallback, and allocation-correlated GPU identity.
- The allocation runner keeps NFD, Controller, Providers, and User inside one
  Slurm job; traps retain original exit status and promote logs/profile output.
- Permission, NAC-ABE, one-time UserToken/ProviderToken, replay, and provider
  permission paths remain enabled. The packaged security contract rejects
  obvious bypass text and verifies the required framework token paths.
- No password, MFA value, private key, registry token, prompt, or tensor payload
  is intentionally included in evidence. The final bounded scanner reports
  zero findings.

## Validation record

| Gate | Result |
|---|---|
| Waf incremental build | PASS, 305 targets |
| Full C++ unit executable | PASS, 257/257 test cases; 7 conditional bodies not executed |
| Spec 109 container Python units | PASS, 45/45 |
| Spec 109 Python lineage/source/token/manifest | PASS, 10/10 |
| Python `ServiceResponse.request_id` binding | PASS, 2/2 after in-place wrapper rebuild |
| Allocation all-process readiness | PASS, 1/1 |
| Packaged security contract | PASS, 1/1 |
| Local/`/home` model sentinel | PASS, 1/1 |
| ShellCheck | PASS, zero diagnostics |
| MiniNDN preflight | FAIL retained: stale binding first; after rebuild, `local deadline` |
| Matrix analysis | PASS, 105/105 represented and terminal `BLOCKED` |
| Final secret scan | PASS, 0 findings |
| Strict Spec Kit structure | PASS: 54 FR, 22 SC, 6 stories, 165 tasks |
| GSD | healthy; one unrelated in-progress Spec 107 plan has no summary |
| Agent context | updated to `specs/109-ndnsf-di-itiger-qwen-scaling/plan.md` |
| Quickstart | CLI/help, snapshot, predecessor observation, units, and 105-cell analysis validated |

The C++/Python/Shell logs and exact exit files are retained under
`results/spec109-itiger-qwen/final-tests/`. The frozen MiniNDN failures remain
under `results/spec109-itiger-qwen/preflight/0.5B/`; they were not rerun or
relabelled.

## External evidence and authority limits

- Live read-only iTiger discovery on 2026-07-13 confirmed Apptainer 1.3.4 and
  H100/RTX6000/RTX5000 GRES. Shared free capacity is not treated as user quota.
- A final `sacct` query through the configured `itiger` alias reconfirmed jobs
  146050 and 146123 as `COMPLETED`, exit `0:0`; the durable 0.5B manifest hash is
  `317e5af807b14859dc94c979557cd919fa2871b7112ca4b5c21f27265b0c1496`.
- Only model transfer/seal is measured. GPU oracle/candidate/performance jobs:
  zero. No TTFT, inter-token latency, throughput, percentile, confidence
  interval, overhead, scale, CUDA-stage, or production claim is admissible.
- Reproduction is `INCONCLUSIVE`; physical production is `DEFERRED` to Spec
  106. The 72B and multi-node constraints are retained independently.

## Mandatory workflow gates

| Gate | Use and result |
|---|---|
| Context | Read AGENTS, constitution, Spec 109 artifacts, prior audits, evidence, and active plan; synchronized agent context. |
| CodeGraph | Synced and traced runtime evidence/readiness callers before and after remediation. |
| Spec Kit | Strict structure, analyze, post-implementation audit, traceability, and terminal task accounting completed. |
| GSD | Installation/help and health validated; resumable state is healthy. |
| ARS experiment workflow | Used for controlled baseline/candidate separation, confound control, sample thresholds, confidence intervals, failure retention, and authority grading. |

## Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Storage-safe iTiger Qwen campaign and honest negative closure match the request. |
| Architecture and ownership | Yes | Spec 107 runtime, Spec 108 deployment, Spec 109 experiment, Spec 106 physical ownership remain separated. |
| Security/correctness | Conditional | Contracts and negative tests pass; live MiniNDN/GPU acceptance is blocked. |
| Task executability | Yes | Canonical repository CLI, templates, scripts, exact paths, and terminal rules exist. |
| Validation/evidence | Conditional | Offline coverage is strong; no GPU inference measurement exists. |
| Migration/rollback | Yes | No protocol migration; candidate identities are immutable and measured work is never retried. |
| Code reality | Yes | Audit-found runtime evidence/readiness/concurrency gaps were fixed and rebuilt. |

## Next actions

1. Close Spec 107 T027/T028-T038, beginning with the retained MiniNDN `local
   deadline`; do not reuse the failed Spec 109 preflight as a new measurement.
2. Close Spec 108 T091-T102 and publish an accepted, digest-bound GPU
   OCI/SIF/deployment release.
3. Re-observe exact predecessors, recapture the now-changed source, and create
   a new candidate/campaign identity before any standalone or candidate job.
4. Execute the frozen 0.5B oracle → artifact → staged baseline → NDNSF-DI
   candidate sequence exactly once; advance larger sizes only through scoped
   gates.
5. Keep real-network/UAV, production security, performance/soak, and physical
   deployment work in Spec 106.
