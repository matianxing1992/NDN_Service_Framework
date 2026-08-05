# iTiger Qwen evidence and acceptance

This document defines what may be claimed from an iTiger Qwen run. The
operational sequence is in [itiger-qwen-models.md](itiger-qwen-models.md).

## Evidence planes

Qwen evidence has four independent planes:

1. **Standalone oracle** — full-model deterministic tokens and capacity only.
2. **Artifact correctness** — stage boundaries, tensor/digest continuity,
   CUDA loadability, logits, and exact-token equivalence against the oracle.
3. **Matched staged baseline** — the same artifacts, runtime settings,
   workload, stage/GPU map, warmup, logging, timeout, and measurement window as
   the candidate, without NDNSF networking/security/orchestration.
4. **NDNSF-DI candidate** — normal permissions, NAC-ABE, one-time tokens,
   replay protection, collaboration assignment, dependency dataflow,
   node-level CUDA assignments, GPU UUIDs, and final exact output.

Only plane 4 establishes NDNSF-DI candidate correctness. Candidate overhead is
candidate minus plane 3, not candidate minus Transformers reference timing.
Physical production remains a separate acceptance scope.

## Required immutable identity

Every live cell retains:

- submission ID, run ID, Slurm job ID, and predecessor/replacement link;
- source bundle, Git/source state, job script, SIF, model revision, tokenizer,
  stage artifact, prompt set, and analyzer digests;
- Slurm request, assigned nodes, GPU models and UUIDs;
- project and scratch capacity decisions;
- start/end time, scheduler state, exit code, and original stdout/stderr;
- evidence root and a checksum manifest.

Rendering never submits. A started formal identity is not edited, retried in
place, or overwritten.

## Gate acceptance

### Runtime and placement

- candidate SIF digest matches the frozen manifest;
- no mounted replacement `.so`, pybind, or vendor-site path;
- host provides the driver; SIF provides a coherent user-space runtime;
- every stage uses an allocated GPU UUID and records `cpuFallback=0`;
- CUDA unavailable, unprofiled fallback, allocator corruption, or native API
  mismatch fails closed.

### Network and secured collaboration

- the allocation-scoped multi-node NFD probe passes before model execution;
- controller, user, and all providers use the normal permission/token path;
- the real collaboration assignment and `report_operation_status()` path run;
- no debug authorization bypass is present;
- no private key or bootstrap token is retained in promoted evidence.

### Model and dependency correctness

- the immutable model/tokenizer/chat-template/decoding contract matches the
  standalone oracle;
- stage ranges cover the intended layers once, without overlap or gaps;
- every producer output matches the corresponding consumer input by Data name,
  bytes/segments, and SHA-256;
- final token IDs match the reference in order;
- a complete-answer claim requires EOS within the frozen token ceiling and
  decoded text, not only one matching top token.

### Repeated-generation measurement

- warmups are retained but excluded;
- successful measured rows are kept separate by prompt and repetition;
- failure, mismatch, timeout, cancellation, and truncation rows remain evidence
  and never enter successful percentiles;
- per-token timing reconstructs TTFT and inter-token latency;
- per-generation timing reconstructs total latency, output length, and
  tokens/s;
- percentile and confidence claims are withheld when sample size is
  insufficient.

For rate-series performance cells, retain each original 60-second repetition
separately unless the frozen feature contract defines a different bounded
generation campaign. Treat p50/p95/p99 as unavailable below 20/100/1000
observations respectively; a confidence interval never rescues an invalid
cell.

## Scheduler result versus workload result

Keep these two facts separate:

```text
formal job result = scheduler terminal state + original exit code
workload result   = what immutable model/network/security evidence completed
```

A post-run analyzer can fail after a valid workload. In that case:

1. preserve the formal job as failed;
2. preserve the partial evidence directory and original logs;
3. classify the failing analyzer assertion;
4. run any corrected analyzer read-only against the same bytes;
5. state exactly which bounded workload claims the immutable evidence supports;
6. do not promote, relabel, overwrite, or silently rerun the failed identity.

Spec 160 Job 174221 is the reference case: the three-node workload completed,
but a post-run assertion compared outer ID `spec160-qwen-live-015` with correct
indexed request ID `spec160-qwen-live-015-0`. It remains `FAILED (1:0)`, while
its immutable evidence supports only the documented single-request capability.

## Minimum evidence by claim

| Claim | Minimum evidence |
|---|---|
| Runtime binding works | local Docker security smoke plus single-node SIF operation-status smoke |
| Cross-node transport works | allocated-node TCP/UDP NFD probe |
| Stage artifacts are valid | exact ranges, digests, CUDA loads, no fallback, oracle equivalence |
| NDNSF-DI layer pipeline works | three physical stage receipts, two dependency digest chains, secured final response |
| Complete answer works | exact ordered sequence, EOS, decoded text, per-token correlation |
| Latency distribution | frozen warmup/repetition plan and retained raw successful/failed rows |
| NDNSF-DI overhead | candidate and matched staged baseline |
| Scaling or production readiness | a separately designed campaign; never inferred from one correctness run |

## Result vocabulary

Use one of these labels:

- `IMPLEMENTED`: code and local contract tests exist; no live platform claim.
- `MEASURED`: a bounded live measurement exists for the stated gate.
- `PASS`: all frozen acceptance gates for that exact cell passed.
- `PARTIAL_WORKLOAD_ACCEPTED`: formal job failed, but immutable evidence supports
  explicitly bounded workload claims.
- `FAILED`: the original cell failed; preserve the cause and evidence.
- `BLOCKED`: a prerequisite or capacity gate prevented execution.
- `PENDING`: submitted but not started/completed; no execution claim.

Never translate `PENDING` into “in progress” model computation, or
`IMPLEMENTED` into “tested on iTiger.”

## Current checkpoint (2026-07-28)

| Scope | State | Accepted statement |
|---|---|---|
| Spec 162 Qwen3.6-27B RTX-only design | `PROPOSED` | three-node/three-stage, 64-token, five-by-five experiment is frozen; current Qwen2-only adapter and unmeasured 32 GB fit block live execution |
| Spec 160 three-node 0.5B request, Job 174221 | `PARTIAL_WORKLOAD_ACCEPTED`; formal `FAILED (1:0)` | one secured request used three RTX 5000 Ada GPUs, two matched hidden-state transfers, exact top token 27024, zero CPU fallback |
| Spec 161 capacity probe, Job 174363 | `PASS` | allocation scratch was writable and large enough for the frozen temporary-peak estimate |
| Spec 161 generation implementation | `IMPLEMENTED` | bounded EOS generation and evidence/analyzer contracts pass focused local tests |
| Spec 161 32B preparation, Job 174382 | `PENDING` at recorded checkpoint | exactly-once job was queued; no model download, CUDA reference, stage promotion, or distributed generation may yet be claimed |
| Spec 161 formal multi-prompt campaign | `BLOCKED`/unauthorized | no 32B distribution or performance result exists |

The checkpoint is historical. Recheck Slurm and promoted evidence before using
it as current status.

## Current TigerCluster requalification boundary (2026-08-01)

The first explicitly authorized Qwen3.6-27B run used the native SIF
`sha256:5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5`,
the immutable three-stage manifest
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`,
and three RTX 5000 nodes. Jobs 181527, 181528, 181530, 181531, and 181532 are
preserved `FAILED` identities whose causes occurred before token generation
(path mapping, tokenizer binding, source-bundle API compatibility, and finally
a Provider/Core boot-epoch mismatch). No complete-answer or latency-
distribution claim is accepted.

Every attempt did reach three-stage DistributedRepo publication. That is a
bounded publication-path observation, not a `PASS` and not a throughput
measurement. The 53.79 GB cold path showed a material interval between all
payload bytes being staged and the root-manifest/catalog registration becoming
visible. Reports must split payload transfer, ACTIVE/catalog commit, Provider
fetch/cache, GPU preparation, and inference. Spec 167's controlled transport
campaign remains the authority for repository goodput; a Qwen job's total
elapsed time must not be substituted for it.

Follow-up identities are retained separately: 181538/live-009 found the
frozen ACK TTL mutation; 181539/live-011 required binding the current DI
provider module into the sealed SIF; 181541/live-012 exposed invalid
`DIRoleAssignmentV2.artifact` cache-key access; 181543/live-013 then exposed
duplicate DI GPU admission on Selection (the same offer was held twice); and
181544/live-014 carries the idempotent offer-reuse fix. None is a
complete-generation PASS until its promoted `result.json` and generation
analyzer pass.

## Request-driven preparation rule (2026-08-02)

The previous Qwen harness published the stage objects and then slept for a
fixed Provider/User settle interval. That is not an NDNSF-DI correctness
condition: it hides whether a real Request reached ACK collection and mixes
deployment convergence with model-fetch latency. The required order is now:

```text
Request -> ACK_CLOSED -> graph/candidate strategy -> selected split materialize
-> DistributedRepo root-manifest publication -> commit_plan/Selection
-> Provider-local model fetch/load -> Stage 0 request-ready execution
-> later stages predecessor-data-ready execution -> Response
```

The default Qwen smoke starts with a sealed stage/graph manifest in
`REQUIRES_DISTRIBUTED_REPO_REGISTRATION` state. It must not contain real model
Data names before the first Request. The User materializer validates the
candidate after ACK closure, invokes the normal bounded Repo `publish_file`
path, and commits Selection only after all three exact receipts exist. The
Provider reads that registration at Selection preparation time and retains the
content-addressed stage in its disk/GPU cache for later token requests.

Evidence must contain the ordered User markers
`REQUEST_SENT < ACK_CLOSED < ARTIFACTS_READY < SELECTION_COMMITTED`, plus
per-role `STAGE_EXECUTION_READY` and (for stages 1 and 2)
`STAGE_DEPENDENCY_WAIT/READY`. Fixed settle sleeps are prohibited as a
correctness gate. The active harness also records
`SPEC162_REQUEST_GATE_OPEN` immediately before the first Request and rejects
source bundles containing the historical settle variables or a literal
`sleep 300`. This is a DI application lifecycle rule; it does not add a new
primitive to base NDNSF.
