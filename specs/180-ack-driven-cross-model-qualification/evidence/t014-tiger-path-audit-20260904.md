# T014 Tiger-path follow-up — 2026-09-04

Verdict: **BLOCK**. This is a current-source blocking-path review and focused
repair, not a completed full T014 convergence audit or qualification campaign.
Authority remains revision 123 with the revision-112 single-YOLO scope.
HEAD: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` (dirty worktree preserved).

## Verified and repaired

| Finding | Actual path and correction | Evidence level |
|---|---|---|
| Tiger still launched Python Providers | `packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py:render` emitted four `provider.py` commands although the local runner uses `di-native-provider`. It now emits four independent native `--serve` commands, one role each, three CUDA model roles and a CUDA-hidden CPU Merge. No preassembled local model path is passed. | Rendered argv verified, not GPU execution |
| Repo command did not exist | Renderer referenced `NDNSF-DistributedRepo/pythonWrapper/repo.py`, which does not exist. It now uses the maintained YOLO `repo_node.py` with explicit Repo identity, storage and bounded thread settings. | Path/argv verified, not live startup |
| Old result profile | `scripts/validate_spec180_results.py` required two requests and three distinct device IDs, and allowed Qwen. It now requires one YOLO request, one shared physical GPU, four role/PID/backend/device records, CPU-only Merge and the exact eight-child inventory. Boolean counts and duplicate PIDs are rejected. | Focused positive/negative tests |
| Hash-only oracle attestation | Validator previously hashed referenced bytes without comparing those bytes to asserted PASS fields. It now requires a JSON oracle body whose schema/version/status/candidate/fields exactly match the reference after hash verification. | Forged-field mutation rejected |

The stricter terminal record encoding is documented in
`contracts/cross-model-qualification-v1.md`. It implements existing
FR-017/018/019/024 requirements; it does not introduce a new experiment or
change ACK/placement ownership. Rendered argv does not prove that the GPU EP,
native assembler, or Merge actually executes correctly on Tiger.

## Controlling gaps still open

| ID / severity | Source evidence | Owner / required closure |
|---|---|---|
| TP-01 HIGH | `run-ndnsf-yolo.sh` starts Repo and Providers immediately after Controller; it waits for a publication receipt only later. Native argv depends on generated plan/manifest/trust files. There is no full Controller → Repo → Provider → User readiness chain. | T013: use the maintained readiness/supervision semantics, test delayed readiness and early child exit; no later phase may start on failure. |
| TP-02 HIGH | `run-ndnsf-yolo.sh:finish` kills direct PIDs and uses unbounded `wait ... || true`; the success path ignores sibling exit results and emits only `orchestration-terminal.json`. It does not invoke the structured result validator. | T013: bounded owned-process-group cleanup; collect every status; produce fresh candidate-bound protocol/numerical/runtime/redaction/cleanup files from real evidence and invoke the validator. Test survivor, child-failure, missing/tampered oracle and timeout paths. A result marker alone must never qualify. |
| TP-03 HIGH | `run-functional.sh` probes `/opt/ndnsf/bin/run-ndnsf-yolo.sh` but executes the renderer/launcher under `/bundle/packaging/...`. Bootstrap is also bundle-loaded. | T013: reconcile the actual executed immutable source/harness/submit planes with the probed subject; reject mismatch before external effects. Do not claim in-image/no-overlay qualification from the current probe. |
| TP-04 HIGH | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_run_y_n_matrix` runs Y-N-O live but routes the remaining subcases through `_run_focused_y_n_negative`, then emits aggregate PASS. | T011: implement the critical registered real-network negatives; keep focused checks distinct from FR-014 local qualification. Test aggregate rejection when any required live control is absent. |
| TP-05 HIGH | Source-only archive below omits build/runtime/dependency/SIF and separate design/harness result-validator inputs. A `CandidateRecord` schema permitting dirty bytes does not prove those planes are bound. | T003/T013: bind exact current source delta plus all required planes; repeat full T014 against that subject. No commit-only prerequisite, no invented digest or reuse of stale seals. |

These are sufficient to BLOCK formal validation. Other FR mappings still need
the complete T014 pass after the last repair; unexamined paths are not marked
PASS by this report. Historical rev-116/rev-123 reports and invalidated SIF
records were not overwritten. No new signature, key, SIF, remote mutation,
scheduler job, or live network campaign was created.

## Focused verification

Initial new contract regression: **10 failed, 3 passed** against the old result
validator and repaired renderer. After repair: **13 passed**.

Final command (implementation regression only):

```bash
python3 -m pytest -q --tb=short \
  tests/python/test_spec180_tiger_contract.py \
  tests/python/test_spec180_release_workflow.py \
  tests/python/test_spec180_candidate.py \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_ndnsf_di_provider_v3_boundary.py
```

Result: **118 passed in 1.15s**. An earlier invocation named nonexistent
`test_spec180_candidate_closure.py` and collected nothing; it was corrected to
the actual `test_spec180_candidate.py` above. This is not T015 or evidence of
CUDA, MiniNDN, SIF or Tiger execution. Relevant `git diff --check` passed.

SHA-256 of changed implementation/test inputs (without the `sha256:` prefix):

| File | SHA-256 |
|---|---|
| `scripts/validate_spec180_results.py` | `11ded888b3c13e01f1e8be40bb9902b315089c3d0a1583081d864970982638a9` |
| `packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py` | `2d4d570bb068c208f77205811e2c0c72280250ba02dbbfedd9956e3bdcb80670` |
| `tests/python/test_spec180_release_workflow.py` | `a2cf4df60618e071bb739794c5a17b2dca23f52abb7344a8d37df36b1a7c9ed7` |
| `tests/python/test_spec180_tiger_contract.py` | `45ed74c66cd790daa919102e270c31b845cdf5400d85fe6ccf0dc9f8842aa12b` |

## Source-only checkpoint, not a candidate seal

Used the existing `prepare-local-sif-source.py` and
`validate-local-sif-source.py` helpers under
`packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/`.

- Directory: `.codex-tmp/spec180-source-checkpoint-20260904-ypOcFg/`
- Archive: `workspace.tar`, 406 files, 15,769,600 bytes, zero compiled payloads.
- Archive SHA-256: `sha256:9e4b861b6d3289c214e7a44d1610754ebb37844a46b63d20a9108593737acb6e`
- `source-seal.json` semantic digest: `sha256:cb7be5b59f07d3a33322456f4607ca725ac4ca79febf7363941ea84ca53686c1`
- Source mode: `sealed-current-worktree-files`; archive preflight PASS.
- Includes the current Python Provider repair and native Tiger renderer.
- Dependency archives are empty. The runtime-source allowlist does not include
  `scripts/validate_spec180_results.py`, these focused tests, or Spec180 docs;
  the hashes above are a scoped checkpoint, not a complete candidate identity.

Do not use this archive to bypass TP-01 through TP-05, start a SIF build, or
promote a candidate. Later behavior changes require a new source checkpoint.

## Workflow gates and continuation

Context Mode project/active health passed; repository documents remained
authoritative. CodeGraph was used first; archived duplicate symbols required
exact canonical-path verification. Spec Kit implementation/audit gates were
used. GSD progress was inspected, but its historical Spec170 resume state was
not allowed to override active Spec180 tasks. ARS is not applicable to this
implementation-only repair. No unrelated provider/configuration was enabled.

Next implementation checkpoint: close T013's real supervision/result chain
(TP-01/02/03), then T011's real Y-N negatives (TP-04), complete candidate-plane
binding, and rerun full T014. Only a fresh PASS can unlock T015 → SIF → Tiger.
