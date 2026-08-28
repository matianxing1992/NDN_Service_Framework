# Spec 168 final audit

## Verdict

**CONDITIONAL PASS for the qualified small-model and lifecycle scope; BLOCKED
for full Spec 168 completion.** The local and exact-SIF gates, small-model
control, 30-row cold/warm schedule, failure taxonomy, and bounded TigerCluster
operator policy are evidenced. The required large-model response and clean
allocation reproduction are not evidenced and are intentionally not retried.

## Gate record

- Structure check: `audit_speckit_structure.py --strict` → PASS (27 FR, 12 SC,
  17 tasks; T013 is now closed, while T015/T016 remain open).
- Python Spec168 suite: `87` tests pass with one explicit skip. The skip is the
  real tiny-Qwen execution test because this host's Transformers `4.46.3`
  installation does not expose the Qwen3 module; the exact locked runtime gate
  remains the authoritative environment for that materializing check. The
  failure-taxonomy, large-model bounded gate, numeric-equivalence,
  cold/warm-analyzer (10 tests), Qwen3.6 preparation (12 tests), and all other
  contract tests pass.
- Exact SIF/CUDA Gate C v93: PASS, RTX 5000, result digest
  `sha256:e36fa1a6fc78191bd8dfc599a7edd4063e1d5751c59e31fbc82e538d8126c0bf`.
- Small control Job 182518: PASS, one wire Request, one authenticated
  response, three CUDA stages, 47 tokens, zero CPU fallback.
- Repeated small Job 182777: derived analyzer PASS on 30/30 rows, 1 cold + 29
  warm, zero token Requests, zero CPU fallback; raw Slurm FAILED state remains
  immutable because analyzer v92 rejected concurrent stdout interleaving.
- Large Job 182780/v93: OUT_OF_MEMORY at rank step after verified Stage-0
  transfer; no generation or terminal Response. The remote failure directory is
  retained under `evidence/spec168/tiger-large-single/`.
- The one FR-019 resource-repaired replacement, Job 182782/v94, used the frozen
  `96 GiB/node`, three-RTX-5000 profile. All three shards fetched and all three
  Providers reached `RUNTIME_READY` on CUDA with `cpuFallbackCount=0`. The
  runtime then returned one authenticated 8,468-byte response with one wire
  Request and zero token Requests, but the exact deterministic reference check
  failed (`TOKEN_MISMATCH`, 64 tokens, `exactReferenceMatch=false`), so the
  Stage-0 rank returned code 1. The job has `terminal-state=FAIL` and no
  analyzer output; it is retained as an explicit preparation-success/
  deterministic-response-failure boundary. No further large-model identity is
  permitted in this campaign.

## Open items and prohibited claims

T013 is closed by `evidence/defects/closure-matrix.md`. T015 is not complete
because SC-008 is unmet: v94 reached three CUDA runtime-ready checkpoints and
returned an authenticated response, but failed exact deterministic reference
acceptance. T016 is not run because it requires an
accepted large profile and the campaign contract forbids another large-model
retry after the v93 OOM and v94 response failure. Therefore do not claim full
Spec 168 completion, large-model success, or clean-allocation reproducibility.

The TigerCluster policy is now agent-selected and local-first: the agent may
submit a bounded remote job without asking again when real GPU, node, or model
scale is required; ordinary debugging and mobility evidence remain local. Every
failed identity is retained and no unbounded background retry is allowed.

## Recommended next step

Keep T015/T016 open and converge the milestone around the accepted small-model
scope. If large-model qualification is required in a future milestone, first
diagnose the v94 response-boundary failure and change the resource/runtime
contract under a new explicitly approved Spec; do not resubmit either 182780
or 182782 in Spec 168.
