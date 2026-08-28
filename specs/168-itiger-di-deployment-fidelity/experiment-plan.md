---
material_passport:
  origin_skill: academic-research-suite/ars/experiment-agent
  workflow_mode: plan
  generated_at: 2026-08-03
  verification_status: UNVERIFIED
  intended_use: NDNSF-DI deployment-fidelity implementation and acceptance plan
  claim_boundary: fixed TigerCluster deployment, pinned models, prompts, and candidate identities
---

# Experiment Plan: Deployment-Faithful NDNSF-DI Qualification

## 1. Research questions

**RQ1**: Can the pinned NDNSF-DI candidate complete one secured three-node GPU
invocation from Request through an authenticated multi-token Response while
preserving one request identity and using no CPU fallback?

**RQ2**: Does execution follow local preparation and direct data dependencies,
rather than a global all-role-ready barrier, fixed settle interval, or per-token
collaboration?

**RQ3**: When compatible shards remain on Providers, does post-ACK planning
select that residency and eliminate duplicate model transfer and device load on
subsequent invocations?

**RQ4**: Does the same lifecycle complete for a pinned model whose full weights
exceed one target GPU's usable memory?

**RQ5**: When an invocation fails, can retained evidence assign one primary
lifecycle boundary and last-progress checkpoint before any repair is attempted?

## 2. Hypotheses and endpoints

### Primary correctness hypotheses

- **H1**: Every accepted small-model invocation has exactly one request ID across
  all lifecycle records and one authenticated complete Response.
- **H2**: Each stage starts after its own GPU-ready event and all direct input
  events, with no global readiness/activation prerequisite.
- **H3**: Every compatible warm invocation has zero duplicate model-payload bytes
  and zero redundant device loads for unchanged assignments.
- **H4**: One large-model invocation completes the same lifecycle with three CUDA
  stages and no CPU fallback.

These are deterministic contract hypotheses. One counterexample fails the
corresponding gate; null-hypothesis significance testing is not used to excuse a
contract violation.

### Primary endpoints

1. terminal-kind and complete authenticated answer;
2. request/attempt/plan identity consistency;
3. causal stage readiness/dependency ordering;
4. CPU fallback count and recorded CUDA devices;
5. duplicate payload bytes, Repository segments, and device load count;
6. schedule-row completeness and failure classification.

### Secondary descriptive endpoints

TTFT, per-token latency, total latency, tokens/s, ACK collection, planning,
publication, fetch, disk-to-RAM, RAM-to-GPU, dependency wait, execution, response,
and cleanup duration. They characterize the fixed deployment; no population-wide
performance claim is planned.

## 3. Experimental units and design

The experimental unit is one durable invocation, not a token, stage, Slurm step,
or successfully completed subset. The blocking context is one unchanged
three-node allocation with one Provider/GPU stage per node.

### Factors

| Factor | Levels | Role |
|---|---|---|
| Model scale | Qwen3-0.6B; pinned larger Qwen | validation ladder |
| Cache state | cold; disk; RAM; GPU; mixed/invalid | observed treatment state |
| Prompt | five pinned real prompts | workload blocking factor |
| Repetition | one warmup; five measured | within-prompt characterization |
| Stage | 0, 1, 2 | causal/dataflow evidence |

Cache state is not inferred from repetition number. It is assigned from actual
residency, transfer, and load evidence for that invocation.

### Controls

- Job 181948 is the retained small-model positive control.
- Job 181951 is the retained large-artifact-fetch negative control.
- A single-stage reference adapter output under the same model/tokenizer/prompt
  is used for deterministic output/token equivalence where available.
- Security-negative and wrong-binding regressions verify fail-closed behavior.

## 4. Fixed material identities

The campaign manifest records source tree/patch, SIF, Python/C++ bindings, model,
tokenizer, graph, shards, DistributedRepo manifests, strategy, prompt set,
generation policy, topology/routes, policies/certificates, Slurm scripts,
allocation/GPU inventory, environment, and analyzer digests.

Existing matching material is reused. Rebuilding or repartitioning changes the
candidate and therefore requires a new manifest; it is never routine cleanup.

## 5. Pre-TigerCluster admission

### Gate A - focused regressions

At minimum:

- `tests/python/test_ndnsf_di_selection_dataflow.py` for post-ACK planning and
  dependency execution;
- `tests/python/test_spec165_progress_deadline.py` for hard/no-progress bounds;
- `tests/python/test_spec161_qwen_generation.py` for one-request multi-token
  generation;
- new Spec 168 regressions for per-role activation, request binding, cache tier
  validity, zero-copy/cache views, adapter-confirmed GPU residency, stalled Repo
  ranges, wrong-attempt outputs, first-writer terminal state, and evidence
  reconciliation.

### Gate B - real MiniNDN

Run Controller, Repository, User, and three independent Provider processes over
real MiniNDN/NFD faces and routes. On the 8 GiB development host, use a frozen
three-role tiny-Qwen fixture under `--memory=6g --memory-swap=7g`; it must retain
two real dynamic dependencies, production lifecycle/security, real
DistributedRepo transfer, real transformer adapter calls, and complete
multi-token output while recording zero cgroup OOM events. Its model identity is
deliberately distinct from the remote Qwen3-0.6B control. No in-process mock,
fake face, shared-filesystem payload injection, or test-only request ID.
On a host without CUDA this gate declares `deviceClass=CPU_LOGIC`; that validates
logic and process/network fidelity only and cannot be cited as GPU evidence.

### Gate C - exact candidate container

Use the exact SIF and command/environment contract intended for TigerCluster.
Verify bindings, stdin/config delivery, routes, repository payload identity,
CUDA visibility (when available), scratch capacity, lifecycle writer, and
analyzer. Do not rebuild the foundation.
The local exact-SIF logic pass may use the same bounded tiny-Qwen fixture; it
qualifies source/container fidelity, not Qwen3-0.6B capacity.
Before Stage E1, rerun this exact SIF in a bounded single-node Slurm allocation
with `deviceClass=CUDA` and `--require-cuda`. That preflight is container/device
admission, not a three-node inference result.

### Gate D - immutable candidate audit

Require all hashes and schedule rows, no mutable source overlay outside the
recorded candidate, enough scratch/GPU capacity, preserved prior evidence, and a
clean destination directory. A failure at A-D prevents remote submission.

## 6. TigerCluster protocol

### Stage E1 - small-model single request

1. Obtain one Slurm allocation containing three RTX 5000-class GPU nodes.
2. Start the normal NFD/security/Controller/Repo/Provider process topology.
3. Verify process and route bootstrap readiness; do not wait for model
   preparation or a fixed Provider settle window.
4. Submit one deterministic real User request for Qwen3-0.6B with at most 64 new
   tokens.
5. Let ACK closure, planning, final Selection, artifact preparation, and
   data-driven execution occur inside the request lifecycle.
6. Accept only a complete authenticated Response, consistent request ID, three
   CUDA stages, one wire request, no per-token Request, and zero CPU fallback.

### Stage E2 - small-model repeated requests

In the same unchanged allocation, use five frozen prompts. For each prompt run
one warmup and then five measured invocations. Preserve all 30 scheduled rows.
The first request is not automatically labeled cold and later requests are not
automatically labeled warm; classify each from evidence.

### Stage E3 - large-model single request

Only after E2 passes, reuse the pinned large-model artifacts and run one
deterministic prompt through the same public API and lifecycle. Require one
complete Response. Do not launch a large-model repetition schedule until this
gate passes.

### Stage E4 - clean-allocation reproducibility

After E1-E3 pass, release the original allocation and repeat the accepted
small-model and large-model single-request correctness profiles in one clean
three-node allocation using identical immutable identities. Compare terminal
answer/token sequence, plan assignment and dependency order, initial cache-state
classification, stage devices, and security verdict. Any mismatch is retained
and blocks the reproducibility claim; it is not averaged away.

## 7. Progress monitoring and stopping rules

Each publication, fetch, preparation, stage, and response operation has:

- a frozen hard deadline;
- a shorter no-progress deadline;
- authenticated monotonic progress checkpoints;
- a precise terminal failure containing the last checkpoint.

Stop an experiment identity at the first terminal failure, security violation,
candidate mutation, unrecorded route repair, shared-storage substitution,
schedule corruption, or analyzer evidence gap. Preserve partial outputs. Do not
change retry/window/timeout/model/route settings and continue under the same ID.
The sole exception is a pre-inference resource/environment failure that is
proven by Slurm/cgroup evidence after all admission gates pass: admit at most
one new identity with an explicitly frozen resource profile and link it to the
closed negative row. This exception does not permit a second replacement or
any unbounded retry.

## 8. Data collection

Retain:

- Slurm allocation/job accounting and node/GPU/process inventory;
- NFD faces, routes, strategies, namespace ownership, and security verdicts;
- ACK capability/residency snapshots and exact planning inputs/decision;
- Repository manifest, range, segment, unique/wire byte, retry/window/backlog,
  and progress records;
- disk/RAM/GPU transitions and adapter load/device validation;
- per-stage dependency/input/output/start/end/backend evidence;
- full prompt, full answer, token IDs, TTFT, per-token latency and stop reason;
- raw logs, lifecycle JSONL, per-invocation rows, campaign reconciliation, and
  immutable human-readable verdict.

Secrets and private key bytes are excluded; their public identities/digests are
recorded.

## 9. Analysis plan

### Correctness

Run deterministic assertions over every admitted invocation. Report numerator
and denominator for complete responses, identity consistency, causal stage
ordering, GPU-only execution, security, schedule completeness, and precise
failure classification. Any missing evidence is a failure, not an exclusion.

### Cold/warm behavior

Group only by observed cache class and compatible assignment. For each prompt
and group, report all rows plus median, interquartile range, minimum and maximum
for phase latencies, TTFT, total latency, and tokens/s. With five repetitions,
avoid asymptotic confidence claims; optional bootstrap intervals are descriptive
and must expose the raw rows.

Warm reuse is causally supported only when ACK inventory and planning show the
resident assignment and transport/load counters independently show zero repeated
work. Latency is corroborating, not decisive.

### Large-model result

The primary result is binary lifecycle completion plus causal evidence. A failed
fetch remains a fetch result; a stage that never executed has no inference
performance value.

## 10. Bias and fallacy controls

| Risk | Control |
|---|---|
| Simpson's paradox | show prompt and cache-state strata before pooled summaries |
| Ecological fallacy | keep invocation-level claims and rows |
| Berkson/collider bias | retain failures rather than condition on completion |
| Base-rate neglect | report counts and denominators for each terminal class |
| Regression to mean | require local regression plus remote requalification, not one faster rerun |
| Survivorship bias | preserve original failed jobs and partial stages |
| Look-elsewhere effect | freeze primary endpoints before submission |
| Researcher degrees of freedom | freeze prompts, analyzer, exclusions, schedule, and stopping rules |
| Correlation/causation | require direct transfer/load evidence for cache claims |
| Reverse causality | retain ACK inputs and strategy decision for placement claims |
| Selective reporting | reconcile every frozen schedule row and publish failures |

## 11. Reproducibility package

The feature closes only when another operator can resolve the immutable material
manifest, run the local gates, submit the same admitted Slurm schedule under
authorization, and regenerate the lifecycle/campaign summaries without manually
editing raw logs. The package includes exact commands, environment contract,
expected schemas, hashes, accepted and failed rows, analyzer version, and the
requirement-to-evidence traceability matrix.

## 12. Verification status

This document is a preregistered plan. No new Spec 168 TigerCluster campaign has
been executed at the time of writing. Job 181948 and job 181951 are prior retained
evidence and are explicitly labeled as such.
