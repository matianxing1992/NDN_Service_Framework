# Spec 166 Post-Run Audit

## Verdict

`PASS`

Spec 166 achieved its stated external-validation objective. The locally gated
candidate ran the frozen Qwen3 minimum workload through real NDNSF-DI Request,
ACK, Selection, dependency transport, operation status, and Response paths on
three TigerCluster nodes. Standalone job `181085` and distributed job `181096`
both completed `0:0`; their promoted evidence and checksum manifests pass.

This verdict establishes correctness for one small-model, three-Stage pipeline
on three RTX 5000 GPUs. It does not establish large-model scalability,
production throughput, tensor parallelism, or statistical performance claims.

## Closure Evidence

| Dimension | Accepted evidence |
|---|---|
| Local authorization | Gate A-D runs `20260731T155338Z-84787254` and final-source revalidation `20260731T164038Z-d1540ad6`: PASS; prepared artifacts reused by checksum-verified links |
| Candidate identity | SIF SHA-256 `e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a` |
| Standalone gate | Job `181085`, `COMPLETED 0:0`, 8/8 exact rows, three profile-policy-v2 PASS profiles |
| Distributed gate | Job `181096`, `COMPLETED 0:0`, `itiger07-09`, three distinct RTX 5000 UUIDs |
| Generation correctness | 8/8 `OK`, 8 tokens each, 8/8 exact reference matches; full decoded answers retained |
| CUDA execution | 64 timing records per Stage; 192/192 `device=cuda:0`, 192/192 `cpuFallback=0` |
| Request lineage | One shared set of 64 unique application-owned request IDs across all three Stages |
| Data dependency | Per-request SHA-256 output/input equality passed for Stage 0→1 and Stage 1→2; published Data names retained for both intermediate Stages |
| Security hygiene | Source/input/SIF/evidence checksums pass; bootstrap token removed before completion; no private key retained |
| Measured distribution | Six measured rows: median TTFT 103.70 ms, median total latency 704.82 ms, median 11.35 tokens/s |

## Preserved Negative Evidence

The campaign did not erase or relabel failures. Jobs `181070`–`181072`,
`181084`, `181086`, `181088`, `181089`, `181091`, `181092`, `181094`, and
`181095` remain under their original identities. Their root causes produced
specific prevention controls:

| Jobs | Defect exposed | Durable prevention |
|---|---|---|
| `181070`–`181072` | interpreter/import path and strict CPU-EP assumptions | exact sbatch-environment probe; evidence-driven profile-policy v2 |
| `181084` | operator-name-only profile policy rejected valid bounded metadata work | operator plus tensor type/shape checks and CUDA semantic groups |
| `181086` | rank entrypoint not executable | executable-bit unit test and pre-srun check |
| `181088` | host wrapper used a container-only source path | source-root contract and regression test |
| `181089` | sealed SIF application API lacked `request_id` | checksum-bound current API overlay; request ID was not removed |
| `181091` | overlay omitted compatibility manifest | packaging preflight and candidate-SIF import probe |
| `181092` | plaintext bootstrap token survived until the outer gate | token deletion before `user-done` and `rank-complete`; ordering test |
| `181094` | ONNX timing omitted CUDA, digest, Data-name evidence | mandatory structured fields on every ONNX Stage timing record |
| `181095` | Python and native logs interleaved and corrupted evidence lines | one-syscall atomic timing-marker emission and unit test |

These failures explain why a purely simulated local test was insufficient.
They also identify the local-gate gap: deployment packaging, credential cleanup,
and mixed-runtime logging contracts must be exercised before future external
submissions, not only the inference algorithm.

## Five-Tool Gate

- Context Mode: health guard passed; repository artifacts remained authority.
- CodeGraph: verified the current ONNX execution and request-lineage paths
  before source changes.
- Spec Kit: requirements, plan, tasks, contracts, evidence, and this audit were
  kept aligned under Spec 166.
- GSD: installation/health validation passed; campaign state was maintained as
  a resumable, identity-preserving workflow.
- Academic Research Suite: experiment-agent rules governed preregistration,
  exact-once identities, negative-evidence retention, and scope-limited claims.

## Residual Risks and Next Gate

- The three-node run proves a small Qwen3 layer pipeline, not a larger model.
- The sample size is sufficient for functional acceptance, not comparative
  performance inference or confidence intervals.
- The sealed SIF required a checksum-bound source overlay because its packaged
  Python API predated the final request-ID path. A future release should bake
  the accepted source into a new SIF and repeat local Gate A-D plus one small
  TigerCluster confirmation before any larger-model campaign.
- Future local deployment gates should include executable modes, exact package
  closure, post-run secret cleanup, structured ONNX timing fields, request-ID
  propagation through typed tensor bundles, and concurrent native/Python log
  integrity.
