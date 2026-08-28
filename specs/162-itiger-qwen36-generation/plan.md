# Implementation Plan: iTiger Qwen3.6 Three-Node Generation

**Feature**: [spec.md](spec.md)
**Date**: 2026-07-28
**Status**: T009_REQUALIFICATION_IN_PROGRESS — local gates pass and the latest
explicitly authorized TigerCluster run completed one request-first, three-node
CUDA full generation with an exact 47-token EOS answer. The Slurm wrapper's
first attempt failed at a client return-type boundary and its corrected attempt
ran successfully but was rejected by an overly strict post-hoc analyzer; the
same immutable evidence was revalidated with the corrected analyzer. No model
or foundation rebuild was performed. A future formal multi-prompt campaign
must preserve the one-invocation contract and the corrected evidence rules.

## Technical Context

**Model**: `Qwen/Qwen3.6-27B`, revision
`6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`, BF16, text-only

**Runtime**: Python, PyTorch, Transformers 5.14.1, current NDNSF-DI application
SDK, NFD, Apptainer, Slurm

**Reference placement**: one RTX 5000 node with three GPUs

**Candidate placement**: three distinct RTX 5000 nodes, one GPU and one stage
per node; ranges `[0,21)`, `[21,42)`, `[42,64)`

**Generation**: `enable_thinking=False`, greedy, EOS, at most 64 new tokens,
full-context `use_cache=False`

**Campaign**: five prompts; one warmup plus five measured generations per
prompt; sequential offered load

**Metrics**: full answer, exact tokens, TTFT, every inter-token latency, total
latency, output length, tokens/s, p50 and descriptive p95

**Constraints**: no H100, no login-node compute, no CPU fallback, exactly-once
live identity, no automatic retry/cancellation, preserve failed evidence

## Operational requalification update — 2026-08-01

The authorized live path used Qwen3.6-27B, three distinct RTX 5000 nodes, one
stage per node, and the native SIF
`5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5`. The
prepared manifest is
`cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787` and the
three stage sizes sum to 53,792,308,358 bytes. Jobs 181527, 181528, 181530,
and 181531 are immutable failed identities with pre-generation failures; job
181532 is the linked failed correction, also before token generation. Their exact causes and evidence roots
are listed in `quickstart.md` and `evidence/t009-requalification.md`.
Live-003 additionally retained ProviderToken-mismatch diagnostics during Repo
STORE control selections; publication completion does not waive that security
and request-correlation check.

Live-005 exposed the next binding defect: fix-004's sealed `provider.py` used
the synthetic CLI `--provider-boot-epoch` for the ACK/assignment, while Core's
opaque Selection context carried the native `ServiceProvider` process epoch.
`DISelectionParticipant.prepare()` correctly rejected this mismatch, and the
requester then waited until its bounded 3,600,000 ms deadline. The next source
bundle must use the Core property as the sole epoch source and add a startup
equality assertion before any ACK is published.

The live runs establish an important timing boundary. DistributedRepo can
finish staging all three large files while the registration/catalog remains
inactive; only the later root-manifest registration makes the automatic
planning record usable. Therefore a request trace must expose transfer,
registration/ACTIVE commit, provider fetch/cache, and inference as separate
phases. The observed roughly 19–22 minute cold publication interval for 53.79
GB is not a standalone throughput claim. A subsequent warm request must be
measured separately using the same content-addressed stage digests and provider
GPU-cache state.

The source bundle is an atomic compatibility unit: `user.py` and
`llm_pipeline_lib.py` must come from the same sealed revision. Fixing one file
by copying from a newer checkout caused a runtime signature mismatch even
though the container and Repo publication were healthy.

## Request-driven lifecycle correction — 2026-08-02

The Tiger harness previously published all three Repo objects and waited for a
fixed Provider/User settle interval before sending the first request. That
ordering tested deployment convergence rather than the NDNSF-DI lifecycle and
could not distinguish registration failure from model-fetch failure. The
corrected path keeps only process/route barriers, then executes:

```text
Request
  -> ACK_CLOSED
  -> graph/candidate strategy evaluation
  -> selected split materialization and DistributedRepo publication
  -> commit_plan / final Selection
  -> independent Provider model preparation
  -> Stage 0 when model + request input are ready
  -> later stages when model + predecessor data are ready
  -> Response
```

The default Qwen smoke begins with a sealed graph/stage manifest whose catalog
state is `REQUIRES_DISTRIBUTED_REPO_REGISTRATION`; it does not publish model
bytes during startup. The Qwen adapter materializes the sealed stage inputs
after ACK closure, invokes the normal Repo publisher, and reuses its
content-addressed registration on later token requests. Provider registration
is late-bound and is read at Selection preparation time. The evidence analyzer
requires the ordered Request/ACK_CLOSED/publication/Selection markers and
stage dependency-ready markers; fixed settle sleeps are not correctness
criteria. Only local task/input validation may precede Request publication;
model graph inspection and candidate enumeration are post-ACK operations and
are recorded by `NDNSF_DI_AUTOPLANNING_GRAPH_READY after=ACK_CLOSED`. If the
ACK snapshot has no valid Provider offer, the requester fails before any model
split or Repository publication.

The configured ACK collection deadline remains a protocol completeness window:
it bounds how long the generic collaboration layer waits for late Provider
ACKs. It is not a Provider-startup settle and does not authorize preparation;
an early-close optimization would require an explicit role-coverage contract
and must be designed in NDNSF-DI without weakening generic NDNSF semantics.

The active harness writes `SPEC162_REQUEST_GATE_OPEN` immediately before the
first User Request and rejects source bundles containing historical fixed-settle
variables or a literal `sleep 300`. This is an orchestration-integrity check,
not a new readiness barrier: after the marker, ACK closure drives planning and
each stage proceeds independently from its own model/input dependency state.

**Terminology**: Historical "reference/preparation allocation" means the
offline Slurm job that downloads, verifies, and packages model artifacts. It is
not a separate NDNSF preparation service call. After Spec 163 requalification,
request-scoped provider load/warm occurs inside the one inference request via
the final Selection transaction. Each selected Provider prepares independently
after its own committed Selection and starts execution when its local
preparation and incoming dependency are ready. There is no DI
`PreparationCommit` message.

### First request-driven Tiger smoke — 2026-08-02 (Job 181874)

The corrected source bundle (`b1a1bd183064bf178cf5c35d19bf6d8619c5319f2913b8a5e80df6e6fd960b9f`)
was submitted once with the existing SIF and 0.6B stage manifest. The run
proved the control/data-preparation ordering before being cancelled to avoid a
one-hour wait after an unrecoverable handler failure:

```text
REQUEST_SENT
  -> ACK_CLOSED (ackCount=3)
  -> DEFERRED_SPLIT
  -> REPO_PUBLISH_START/DONE
  -> ARTIFACTS_READY
  -> SELECTION_COMMITTED
  -> Stage 0 Repo fetch: 594,357,850 bytes, 78,205 segments, 7,374.90 ms
  -> handler failure: ProviderRuntimeContext.request_id did not exist
```

The failure is an application-adapter defect, not a Repo registration or
fetch-integrity failure. `ProviderRuntimeContext` stores the immutable request
identifier as `ndnsf.session_id`; the local fix exposes a read-only
`request_id` alias at that API boundary. Job 181874 is retained as a cancelled
negative result at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-deferred-20260802T210549Z-001.partial`.
It must not be relabelled as PASS or silently rerun under the same identity.

## Constitution Check

- **Canonical runtime**: PASS. Model behavior remains in the DI application
  adapter; NDNSF Core stays model-independent.
- **Security data path**: PASS. Every token uses normal secured collaboration.
- **CodeGraph first**: PASS. Current Qwen2-only loader and generation flow were
  inspected before planning.
- **Spec-driven durable change**: PASS. A new Spec 162 preserves the already
  submitted Spec 161 identity instead of rewriting it.
- **Right-scope verification**: PASS. Pure adapter/generation contracts are
  local; CUDA fit, multi-node NFD, and model correctness require Slurm.
- **Cohesive tasks**: PASS. Adapter test/implementation/evidence is one outcome;
  live preparation and candidate execution remain separate risk boundaries.

## Architecture

```text
Reference/preparation allocation (one RTX 5000 node, 3 GPUs)
  frozen source + processor
    -> text-only non-thinking oracle
    -> exact tokens/text for 5 prompts
    -> streaming stage packages
    -> per-stage CUDA load and peak-memory gate

Candidate allocation (three RTX 5000 nodes)
  Stage 0 [0,21) + embedding
    -- hidden-state reference -->
  Stage 1 [21,42)
    -- hidden-state reference -->
  Stage 2 [42,64) + norm/head
    -> next token
    -> requester appends token and repeats until EOS/64
```

The Qwen3.6 adapter owns hybrid layer construction and invocation. The generic
NDNSF-DI requester/provider/controller, collaboration assignment, operation
status, and dependency transport remain unchanged.

## Phase 0: Compatibility research

Completed in [research.md](research.md). The controlling gap is explicit:
current source supports only `qwen2`; Qwen3.6 requires `qwen3_5` configuration,
hybrid decoder layers, masks, and a new coherent runtime.

## Phase 1: Design contracts

- [data-model.md](data-model.md) defines immutable run and evidence entities.
- [contracts/model-and-placement.md](contracts/model-and-placement.md) freezes
  model, stage, and runtime behavior.
- [contracts/generation-evidence.md](contracts/generation-evidence.md) freezes
  the five-by-five evidence contract.
- [quickstart.md](quickstart.md) separates local gates from authorized Slurm
  actions.

## Implementation strategy

1. Add local contract tests for `qwen3_5` configuration, state-dict remapping,
   hybrid-layer selection, direct-response prompt formatting, and fail-closed
   runtime version/device behavior.
2. Implement the Qwen3.6 stage adapter without weakening the existing Qwen2
   path.
3. Build and seal a coherent SIF with pinned runtime wheels; run the small
   Docker and single-node SIF operation-status gates.
4. Run an RTX-only no-download capacity probe.
5. After explicit authorization, prepare the reference/stages on one RTX 5000
   node with three GPUs and retain per-stage CUDA peak evidence.
6. Run a three-node one-prompt complete-generation smoke.
7. After separate authorization, run the five-prompt campaign exactly once.

The legacy `DistributedRepo.put()` path materializes the entire object and all
4 KiB signed packets in memory, so it is not admissible for the approximately
18 GiB stage objects. T009 adds `put_file()`/`get_file()`: content-addressed,
bounded chunks plus a small root manifest, with whole-file verification on
reconstruction. Local round-trip and tamper tests pass. The pre-split catalog
may become `ACTIVE` only after a real Repo registration record binds all three
root manifests; a catalog containing invented Data names is not publication
evidence. A deferred catalog may intentionally contain synthetic planning names
until the post-ACK publisher returns real root-manifest names; it must not be
treated as `ACTIVE` before that point.

## Statistical and evidence plan

The primary correctness unit is a complete generation, not a token. The primary
performance unit is one measured complete generation. Report each prompt
separately, then a pooled descriptive view. With five observations per prompt,
per-prompt p95 is descriptive only. Do not report p99.

Raw token epochs and generation rows are authoritative. Summary selection is:

```text
phase=measured
status=OK
stopReason=EOS
exactTokenMatch=true
threeStageReceipts=true
twoDependencyReceipts=true
cpuFallback=0
```

## 181929 evidence and placement-strategy correction — 2026-08-03

The first request-first 0.6B execution reached the intended live path on
TigerCluster. The User produced 47 exact EOS-matching tokens; all three stage
Providers executed on CUDA, fetched their content-addressed stage once, and
then reported 46 GPU-cache hits each with zero repeated Repo fetches. The
per-stage cold fetches were 594,357,850 bytes/8,174.16 ms,
283,192,711 bytes/19,995.33 ms, and 625,825,930 bytes/26,382.06 ms for
Stages 0--2 respectively. The final Slurm state is retained as `FAIL` because
the sealed harness scanned its own source bundle for the literal
`CPUExecutionProvider` marker before analysis; post-hoc analysis of the same
immutable runtime evidence is `PASS`. This distinction must remain in all
reports.

The evidence also separates two concerns that were previously conflated:

* the request-first lifecycle is correct (Request -> ACK_CLOSED -> graph/split
  planning -> one deferred Repo publication -> Selection -> data-driven stage
  execution); and
* the post-ACK strategy was not expressing reuse correctly. It re-evaluated
  the graph and Selection for each token and emitted `GENERATED` even when ACK
  metadata proved all selected shards were already resident on the Providers.

`PreSplitFirstStrategy` now exposes this distinction through
`ArtifactPreparationMode.REUSE_CACHED`. A valid all-role cache match (content,
semantics, graph, precision, backend, boot epoch, cache epoch, freshness, and
deadline pin) is scored ahead of Repo/catalog and new materialization. The
trusted coordinator resolves existing artifact names in this mode and never
materializes or republishes the split. `PRE_SPLIT` remains the Repo/catalog
reuse path; `GENERATED` is reserved for a genuine cache miss. This behavior is
placement policy after ACK collection and stays inside NDNSF-DI; it does not
add a distributed-inference mode to base NDNSF.

The smoke analyzer now treats ACKs as successful logical admissions rather
than resource locks: duplicate/retransmitted ACK diagnostics are tolerated,
ACK/release events may overlap across independent token request IDs, and each
request must still have true ACK coverage, its own role release, complete
stage/dependency receipts, and a deadline-bounded final response.

The formal analyzer is aligned with the same rule. It no longer requires the
legacy `reservationHeld=true` field or a global release-before-next-ACK order;
it requires only per-request ACK -> stage timing -> release coverage. The
formal campaign remains the required evidence for cold-first/warm-afterward
reuse because it exercises multiple sequential requests after the first
ACK-closed placement decision.

The strategy in this section is the post-`ACK_CLOSED` strategy, not an
application-time deployment hook. The Request is sent before model graph
inspection and split enumeration. After the immutable ACK snapshot closes,
NDNSF-DI uses the ONNX dependency graph together with Provider cache/capacity
metadata to choose the split and role assignment. `REUSE_CACHED` is permitted
only when an ACTIVE content-addressed Repo catalog supplies exact Data names;
disk bytes reported by an ACK without that catalog are cache evidence but not a
resolvable artifact reference, so the trusted coordinator must materialize and
publish the selected split before `commit_plan`/final Selection. The three
formal attempts 181930--181932 are preserved as negative evidence because they
reached ACK closure and graph readiness but did not reach Selection under this
boundary.

## Generation invocation granularity

The production NDNSF-DI API SHALL use one durable invocation per complete
generation:

```text
GenerationRequest(full input token sequence and generation config)
  -> ACK_CLOSED -> post-ACK graph/cache strategy -> Selection
  -> distributed prefill and internal autoregressive decode
  -> one complete Response
```

The input prompt is a token sequence and is processed as one prefill workload.
Output tokens remain causally ordered, so Providers may exchange per-step
hidden-state/KV data internally, but those data-plane steps MUST NOT create a
new NDNSF Request, ACK collection, strategy invocation, or Selection. Any
presentation-layer streaming is optional and is derived from this one complete
invocation; it is not a per-token NDNSF exchange.

The current Qwen smoke harness intentionally uses one token-level collaboration
per generated token to stress request IDs, ACK coverage, stage dependencies,
and release accounting. That is diagnostic instrumentation, not the target
application API; its repeated ACK timeout is therefore not an acceptable
production generation design. The 0.6B evidence confirms the infrastructure
path, while the next API iteration must amortize planning and selection over
the complete generation.

## 181946--181948 full-invocation TigerCluster requalification — 2026-08-03

These three identities reused the same RTX 5000 SIF, content-addressed stage
manifest, and Qwen3-0.6B artifacts. No foundation image or model preparation
was repeated.

* **181946** reached ACK closure, deferred Repo publication, Selection, all
  three CUDA stages, and a 47-token EOS computation, but the application
  returned `REQUEST_FAILURE` because the automatic path called
  `AutomaticInferenceHandle.result()`. That method returns the adapter-decoded
  `bytes`, while the caller still expected a raw `ServiceResponse` and read
  `.payload`.
* **181947** reproduced the same runtime with the longer total deadline and
  confirmed the same boundary error in raw evidence:
  `error="'bytes' object has no attribute 'payload'"`.
* **181948** used the one-line client fix (`handle.response()`), produced one
  complete response with `status=OK`, `exactReferenceMatch=true`, 47 EOS
  tokens, and the expected decoded answer. The three GPUs were on distinct
  nodes `itiger09`, `itiger10`, and `itiger11`; each Provider fetched its
  content-addressed stage and reported CUDA residency (`cpuFallback=false`).
  The raw runtime is therefore a complete-generation PASS and has
  `wireRequestCount=1`, `tokenRequestCount=0`, and no per-token ACK/Selection.

The Slurm wrapper retained 181948 as `state=FAIL` only because its then-current
analyzer required a marker that the sealed runtime did not emit and required
the stage-0 terminal marker once per token. Running the corrected analyzer over
the immutable 181948 partial evidence returned `status=PASS` with three stage
receipts, 47 full-progress events per role, and Repo fetch completion for all
roles. The wrapper result is not rewritten; the runtime PASS and analyzer
revalidation are reported separately. This preserves the distinction between
runtime behavior and evidence-tool failure.

## Safety and rollback

- Spec 161 Job 174382 is not cancelled or modified by this plan.
- The historical no-submission gate for an open
  `specs/163-di-collaboration-planning` remains preserved in prior evidence;
  the explicitly authorized 2026-08-02 correction is the single exception
  recorded in the request-driven lifecycle section above.
- Job 175053 remains an immutable `FAILED 1:0` identity caused by the recorded
  missing SDK `PYTHONPATH`; the corrected pre-architecture replacement is
  retired unsubmitted.
- No Spec 162 live job is authorized merely by document creation; each live
  identity requires a separate explicit authorization and is recorded here
  only after submission.
- A started identity is preserved at first terminal state and never retried in
  place.
- The Qwen3.6 adapter is additive; rollback removes its registration and
  artifacts without changing the Qwen2 path or NDNSF Core.
- Cleanup removes only a validated current-job scratch prefix after promotion
  checks. Evidence and unique releases are protected.

Before reactivation, Spec 162 must add a fresh requalification task that binds
a new runtime/evidence identity, registers the three Qwen stage artifacts as an
exact pre-split manifest, proves that the built-in pre-split-first strategy
selected them from validated ACK state, and obtains a new exactly-once live
authorization.

## ARS experiment handoff

### Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-28
- Verification Status: UNVERIFIED
- Version Label: code_plan_v1

**Objective**: Determine whether the current NDNSF-DI secured collaboration
path can generate exact complete Qwen3.6-27B answers across three RTX 5000
nodes and retain repeated latency distributions.

**Primary metric**: complete exact EOS generation rate.

**Secondary metrics**: TTFT, inter-token latency, total latency, tokens/s.

**Monitoring**: Slurm process state, per-rank logs, token-epoch JSONL, operation
status, dependency receipts, CUDA peak memory, and evidence checksums.
