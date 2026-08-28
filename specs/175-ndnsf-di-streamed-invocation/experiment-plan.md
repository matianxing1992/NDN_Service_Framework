# Experiment Plan: NDNSF-DI Streamed Generation Qualification

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-08-21
- Verification Status: IMPLEMENTATION_IN_PROGRESS_FORMAL_G0_G7_DEFERRED
- Version Label: spec175_code_plan_v5_implementation_first

## Experiment Overview

- **Title**: Multi-Provider ONNX token streaming and exact conversation continuation through NDNSF-DI
- **Objective**: Verify that one authorized placement can prefill once, produce
  an ordered local-LLM-like token stream, close with one exact final result,
  transactionally promote the completed turn's request-local state into exact
  Provider-local conversation state, and continue a later fresh Request without
  recomputing its full prefix;
  then functionally qualify the frozen three-Provider Qwen3.6-27B deployment
  with ONNX Runtime CUDA on Tiger and measure whether it reaches 20 token/s.
- **Primary hypothesis**: The implementation produces an exact ordered token
  stream and complete result with one Request/plan/prefill per generation.
- **Performance hypothesis**: The warm three-Provider Qwen3.6-27B pipeline has
  median steady-state output >=20.0 token/s and p95 inter-token interval <=75 ms,
  after a bounded same-artifact control has shown that cached incremental decode
  is faster than full-prefix recomputation.
- **Type**: deterministic protocol/inference validation plus environment-sensitive
  GPU performance qualification.

## Research Questions

1. Does the request-scoped stream preserve exact token order, at-most-once
   application delivery, attempt/plan fencing, and complete terminal closure?
2. Does the distributed generation loop perform prefill once and reuse the
   exact complete provider-local decode-state bundle (attention KV plus any
   recurrent/convolution state) instead of repeating full-context inference or
   placement?
3. Is that state reuse automatic in each production Provider session, retained
   on the assigned CUDA device without transporting state over NDN or
   round-tripping the complete state through host memory every token, and does
   it reduce model-compute time relative to a matched full-prefix control?
4. Do deterministic loss/reorder/duplicate/cancellation cases recover or fail
   exactly as registered without partial success?
5. Does the Normal application path omit Provider identities, derive its role
   map from the signed ACK snapshot, retain one collaboration request ID, and
   disclose the event key only to the final-role Provider?
6. On the frozen Tiger candidate, what components determine TTFT and TPOT, and
   does steady-state output meet the 20 token/s qualification threshold?
7. Can a completed turn finalize and atomically promote its request-local state,
   then can a later fresh Request reuse the same exact Provider-role conversation
   states, perform only delta prefill, and match a full-transcript oracle while
   paused conversations move between GPU and bounded host RAM without sending
   state over NDN?

### Cache-scope boundary for measurement

The first three questions measure **request-local KV-cache** behavior: one
Request/generation performs prefill once, advances through decode epochs, and
releases its state at terminal cleanup unless a successful conversation
promotion takes ownership. Question 7 measures a different
**conversation-scoped KV-cache**: a completed turn is promoted transactionally
and a later fresh Request in the same authenticated conversation may restore
the exact committed prefix through its checkpoint and context epoch. These are
separate stores, identities, lifetimes, quotas, residency transitions, and
metrics. A request-local hit or a full-prefix run with a cache counter cannot
count as cross-request conversation reuse.

## Variables

### Independent variables

- gate/environment: in-process CPU, host MiniNDN with host-built applications,
  host MiniNDN with exact-SIF NFD/NDNSF applications, and Tiger CUDA;
- role plan: 1, 2, or 4 tiny-model roles for lower gates; optional Qwen3-0.6B
  diagnostic; mandatory Qwen3.6-27B per-stage CUDA/cache readiness; and the
  mandatory three-role Qwen3.6-27B CUDA subject for the Tiger result;
- fault case: none, first loss, reordering, duplication, cancellation, stale
  attempt, callback failure, bounded backpressure, opt-in replacement;
- signed Provider capability/cache-residency assignment: baseline or registered
  identity permutation for the local ACK-driven placement case;
- prompt ID: two fixed prompts;
- decode treatment for the bounded G5 control: cached one-token incremental
  decode or full-prefix diagnostic, alternated within each matched pair;
- conversation treatment: first-turn full context, exact appended-input resume,
  unavailable-state fallback, or explicit failure;
- state lifetime: request-local decode hit/commit/terminal cleanup versus
  conversation promotion/retained hit; these counters are never pooled;
- terminal-prefix treatment: already complete or bounded state-only
  `CHECKPOINT_FINALIZE` suffix (last accepted token and/or template control
  tokens), with zero extra application events;
- state residency: GPU-resident, host-resident/prefetched, or evicted/missing;
- process repetition: three fresh processes.

### Dependent variables

- exact token/final-result/transcript verdict;
- success/failure and terminal reason;
- Request, ACK closure, plan, Selection, prefill, decode, End, and Response counts;
- TTFT, inter-token intervals, TPOT, steady-state tokens/s, total latency;
- ORT/activation/sampling/feedback/event/queue component timings;
- retransmission/gap/duplicate/reorder counts;
- decode-state component/hit/miss/recompute/commit/eviction/cleanup and provider
  replacement counts;
- per-role state residence, logical bytes, predecessor/committed epoch, complete-
  state host-transfer bytes/time, and decode input sequence length;
- CPU/GPU/memory/network utilization, CPU model-compute fallback count, and
  bounded CPU shape-control operation counts.
- checkpoint/role-receipt validation, parent/successor context epoch,
  continuation hit/miss/reason, delta- versus full-prefill input tokens,
  residency transition, GPU/host bytes, prefetch/transfer latency, eviction,
  fallback, conflict, and successor-commit result.

### Controlled variables

- code tree, final exact SIF (only after host gates), ONNX, tokenizer, adapter,
  workload, and contract hashes;
- model revisions and role splits;
- language-model-only text artifact closure,
  `decodeMode=single-token-autoregressive`, and `mtpEnabled=false`; the source
  model's vision encoder/projector and optional MTP heads are excluded;
- byte-identical P1/P2 workload, tokenizer/chat-template digests,
  `thinkingMode=disabled`, derived input-token IDs/counts, and 120000 ms request
  deadline, frozen before the candidate SIF;
- adapter-certified stateful ONNX prefill/decode schema and complete
  `DecodeStateBundleV1` component manifest; the full-context reference chain is
  an oracle only and is not pooled with the qualification subject;
- current Qwen3.6-27B artifact precision is FP16; BF16 is not accepted by the
  deployed binding unless a separately tested packed-BF16 path is added;
- export-time PyTorch/Transformers logits are a parity diagnostic only.  The
  deployment oracle (`referenceTopToken` and the token transcript) MUST be
  generated by the same canonical ONNX Runtime CUDA stage chain used by the
  measured Providers; its PyTorch comparison token is retained separately. The
  oracle must use incremental state, the exact tokenizer digest, in-vocabulary
  token IDs, and a nonempty decoded transcript. Job 202864's `useCache=false`
  campaign with an empty decoded transcript is execution diagnostic evidence,
  not this oracle.
- Greedy sampling, maximum tokens, prompt set, request deadline, stream options;
- NDN-SVS version/configuration, Boost, ORT, CUDA, Apptainer, and NFD runtime;
- host MiniNDN, Mininet, Open vSwitch, NLSR, topology, route configuration, and
  namespace-tool versions for G3/G4 only;
- Tiger node/GPU allocation recorded per process;
- no admission control, adaptive event policy, runtime download, or implicit
  fallback.

### Potential confounds and controls

| Confound | Control |
|---|---|
| Cold model/artifact/session preparation | record and exclude one cold invocation per performance process |
| Shared-filesystem model I/O | stage each content-addressed 27B stage once into verified node-local cache before G6; record staging separately |
| Different GPU/node characteristics | record GPU UUID/model/clock/driver; retain process-level values |
| NDN cache and retained event state | fresh request/generation/stream epochs; fresh process groups; report cache counters |
| Prompt/token length | fixed prompt IDs and max-token contract; report actual EOS length |
| Chat-template/thinking drift | pin template digest and disabled thinking mode in workload, Request, and accepted plan |
| Model/tokenizer drift | immutable revisions and content digests |
| Multimodal/MTP subject drift | seal text-only graph inventory and reject any loaded vision or speculative/MTP component |
| Host/SIF ABI drift | SIF-native preflight and remote hash verification |
| Repeating historical Tiger deployment mistakes | compare the rendered launcher with Spec170 D0 Job 189483 and r23 D2b/D2h; require one current-SIF D0-shaped control before model staging |
| Intermittent native exit hidden by a rerun | retain every same-subject attempt; a signal/core/missing result blocks promotion until owned, regressed, and resealed |
| Hidden CPU compute fallback | CUDA-first ORT profile; CPU nodes must be limited to the bounded int64/bool shape-control allowlist; any CPU model-compute node fails the run |
| Adapter oracle mistaken for Provider cache | formal cases must reach the production Provider session and reject harness-managed `state_out -> state_in` feedback |
| Correct tokens hide host cache copies | record logical state bytes and host-transfer bytes/time; qualified CUDA decode permits no complete-state round trip after prefill |
| Cached-versus-full-prefix ordering or thermal bias | alternate treatment order within three matched per-stage pairs after one excluded warmup on the same GPU/artifact/input |
| Cache capacity/concurrency interference | pin in-flight entries, record pool depth/eviction, use fresh identities, and retain every miss/recompute |
| Logging overhead | same bounded evidence profile in all measured runs |
| Retry/fault interaction | one registered fault dimension per case; no mixed faults in primary matrix |
| Stochastic token divergence | Greedy primary workload; seeded fixed-logit unit test only for stochastic sampler |

## Setup

- **Working directory**: `/home/tianxing/NDN/ndn-service-framework`
- **Local entry commands**: exactly those in
  [validation-contract.md](contracts/validation-contract.md)
- **Tiger entry command**:
  `packaging/ndnsf-di-container/jobs/spec175/submit.sh`
- **Local environment**: current system toolchain for C++ unit/integration and
  host MiniNDN qualification; the exact locally built SIF is built once after
  those gates. During G4, host MiniNDN/Mininet/OVS/NLSR still own the topology,
  while the candidate SIF supplies NFD and every NDNSF/ORT application process
  through `/opt/apptainer/1.5.3/bin/apptainer exec --cleanenv`.
- **Tiger environment**: exact promoted SIF under Slurm/Apptainer with one GPU per
  selected Qwen3.6 role. Tiger does not run MiniNDN/NLSR and does not rebuild or
  materialize the SIF.
- The SIF embeds the exact sealed framework, Python SDK, adapter, and workload
  source. Runtime source overlays or replacement-module bind mounts are not
  valid experiment subjects.
- Canonical model stages remain external, immutable, and content-addressed.
  G5 stages them once into a verified node-local cache; G6/G6C/G7 reuse those exact
  cache identities and exclude staging from TTFT/TPOT.
- The tiny CPU fixture and optional 0.6B smoke run are regression gates only.
  They do not substitute for the requested Qwen3.6-27B GPU qualification: the
  primary Tiger result must use the pinned 27B ONNX artifacts, ONNX Runtime
  `CUDAExecutionProvider`, and one GPU per complete Provider role.
- **Dependencies**: frozen by candidate manifest; no network model download.

### Execution ownership boundary

| Gate | Host owns | Exact SIF owns |
|---|---|---|
| G3 | MiniNDN, Mininet, OVS, NLSR, namespaces, routes, topology, native host test build | nothing |
| G4 | the same MiniNDN substrate, topology, replay driver, process supervision, read-only fixture mounts | NFD, Controller, repository, four Providers, User, CPython bindings, NDNSF, ONNX Runtime |
| G4T | Slurm CPU allocation, historical-control delta manifest, exact SIF hash staging, process supervision | real NFD, Controller, User, four Providers, CPU ORT; no MiniNDN/NLSR/build |
| G5-G7 | Slurm allocation, GPU/scratch/project storage, immutable model mounts, SIF hash/stage-once verification | NFD/NDNSF/ORT workload runtime, including G6C GPU/host state movement; no MiniNDN or NLSR |

G4 does not require a self-contained MiniNDN installation, topology, NLSR, OVS,
or `mnexec` inside the image. The experiment is valid only when the manifest
records the exact host-harness/topology/fixture digests, the exact SIF digest,
and the per-node `apptainer exec` commands. Host and in-SIF preflights are
separate; both must pass.

### Cost-ordered execution boundary

The active development loop completes all Core/runtime/API/conversation and
harness/launcher implementation first, using only focused compile,
unit, and integration checks. Formal G0--G2 and the real tiny-ONNX MiniNDN
M01--M14 matrix run once against the resulting frozen host build; they are not
repeated after each patch. No SIF build, SIF preflight/replay, remote upload,
Slurm allocation, or Tiger job may start while implementation remains open or
G3 is missing/failing. Once G3 is `PASS`, build exactly one SIF, then let the
same host MiniNDN harness replay the matrix with NFD and NDNSF/ORT applications
launched from that image. One bounded current-SIF D0-shaped Tiger deployment
control must then reproduce the proven launcher/lifecycle before model staging;
only after G4T passes may G5-G7 run. Any later source/contract/workload change
invalidates the SIF and returns to implementation plus G0; repeated SIF
rebuilds are not an accepted debugging method.

## Inputs

| Input | Path/identity | Description |
|---|---|---|
| Contract | `specs/175-ndnsf-di-streamed-invocation/contracts/` | API, wire, generation, and gate authority |
| Tiny model | `tests/fixtures/spec175/tiny-causal-lm-v1/manifest.json` | deterministic CPU ONNX oracle |
| Small Qwen | `Qwen/Qwen3-0.6B@e6de91484c29aa9480d55605af694f39b081c455` | Optional CUDA wiring smoke only; not the primary result |
| Large Qwen | `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` | Tiger three-role subject |
| Workload | `packaging/ndnsf-di-container/jobs/spec175/workload.json` | fixed prompts, seed, tokens, deadlines, stream options |
| Candidate | `$SPEC175_CANDIDATE_MANIFEST` | immutable SIF/source/runtime/artifact identity |

## Expected Outputs

| Output | Path | Format | Success criterion |
|---|---|---|---|
| Gate manifests | `results/spec175/g*/qualification-manifest-v1.json` | JSON | schema-valid, complete case inventory, honest verdict |
| Per-invocation events | `results/spec175/g*/events/*.jsonl` | bounded JSONL | exact cursor/lineage/timing fields; no plaintext prompt/answer/decode-state/logits |
| Evidence index | `results/spec175/g*/evidence-index.json` | JSON | every retained file has SHA-256 |
| Tiger result | project evidence directory mirrored to manifest | JSON/JSONL | exact SIF/workload hash and Slurm/job/node identity |
| Summary | `results/spec175/g7/summary.json` | JSON | correctness plus TTFT/TPOT/tokens/s/resource distributions |
| Conversation control | `results/spec175/g6c/qualification-manifest-v1.json` | JSON | exact two-turn CUDA resume, real GPU-host-GPU transition, fallback negative |

Canonical results are local evidence until reviewed and reduced into tracked
Spec evidence. Raw repeated debug runs are not committed.

## Monitoring Configuration

- **Local timeout**: 120 s per unit/integration case; 300 s per MiniNDN case;
  45 min for complete G4.
- **Tiger timeout**: 10 min preflight; 30 min per-stage readiness; 90 min
  three-Provider functional; 4 h performance campaign.
- **Idle timeout**: 30 s without a verified lifecycle/decode/event progress
  transition; does not extend the absolute timeout.
- **Process monitoring**: process tree, Slurm state, CUDA OOM, NFD liveness,
  per-role heartbeat, evidence file growth, disk free space.
- **Automatic action**: no automatic retry of a crashed experiment. The runner
  terminates only on its registered hard timeout/cancellation and preserves the
  partial manifest.
- **Disk policy**: use the existing project cleanup/retention policy; do not copy
  content-addressed models or SIFs per run.

## Repetition and Workload Schedule

### Deterministic correctness

- Unit tests: normal suite execution; property/mutation loops use fixed seed.
- Integration healthy cases: three fresh processes; fault cases one fresh
  process each.
- MiniNDN M01-M14: three fresh MiniNDN processes per case (42 total).
- Exact tokens, counts, digests, and terminal reason must match; timing is not
  compared for exact reproducibility.

### Tiger functional gates

- G4T: one bounded CPU/no-GPU current-SIF deployment control with one
  Controller, one User, four Providers, real NFD, Request/four ACKs/provider-
  specific Selection/final Response, all child exits zero, and Slurm
  `COMPLETED 0:0`. It is compared with Spec170 D0 Job 189483 and does not count
  as model readiness or a streamed-generation result.
- G5: one exact-SIF CUDA ORT load/execution/cache-readiness record for each of
  the three pinned Qwen3.6-27B stages plus a bounded eight-token cache-
  effectiveness microbenchmark: one excluded warmup and three alternating-order
  cached-versus-full-prefix matched pairs per stage. It has no end-to-end streamed
  invocation sample; its timing claim is limited to cache effectiveness.
- G6: for each of two fixed prompts, record one cold preparation and run three
  fresh warm processes; six measured streamed invocations total.
- G6C: one registered two-turn conversation plus one paused-conversation
  pressure subject proves request-local terminal finalization, atomic all-role
  promotion, exact fresh-request delta-prefill resume, and one real
  GPU-to-host-to-GPU state transition; one unavailable-role negative is run
  first without fallback and then with sealed full-context fallback. This is a
  bounded functional control and contributes no G7 timing sample.
- G5/G6/G6C use exact digests and zero model-compute fallback. The 20 token/s
  threshold applies only to G7.

### Tiger performance gate

- Three independent processes.
- Each process: one cold invocation recorded/excluded, followed by ten measured
  warm invocations alternating the two prompt IDs.
- Total measured warm units: 30.
- No failed or slow valid unit is excluded.

## Analysis Plan

### Correctness

The experimental unit is one invocation. Report all counts and exact oracle
verdicts. Any missing/duplicate logical token, digest mismatch, wrong terminal
reason, multiple Response, second service Request, fixed Provider map under the
registered capability permutation, decryptable event grant at an unselected or
  nonfinal Provider, stale delivery, or CPU model-compute fallback fails that unit and the
mandatory gate. Full-context recomputation for each healthy token also fails
G6, even if its final tokens match the oracle, because it does not exercise the
registered incremental decode-state contract.

For continuation cases, the experimental unit is one turn. A second turn fails
unless it has a fresh Request/generation, validates the exact parent checkpoint
and full role set, processes only appended input before decode, matches the
full-transcript oracle, and commits one linear successor. Missing or mismatched
state must produce the registered explicit fallback/failure with zero
incompatible runner calls. G6C additionally requires measured real
GPU-to-host-to-GPU state movement on every role; CPU residency emulation cannot
substitute for it.

For G5, the experimental unit is one matched stage/treatment pair. Both
treatments use the same artifact, precision, GPU, stage input/oracle, token
prefix, and process. Cache effectiveness passes only when output parity holds,
the cached path consumes one new token, state epoch/length advances, no complete
state host round trip occurs, and cached median model-compute time is lower than
the full-prefix diagnostic for every stage. Report all three pairs and the
per-stage paired ratio; do not pool stages or discard an unfavorable pair.

### Performance

Primary metric: per-invocation steady-state tokens/s after first-token delivery
and before terminal drain. Secondary metrics: TTFT and inter-token intervals.

Across the 30 measured G7 invocations report:

- count and failures;
- min, median, mean, p95, p99, maximum;
- process-level values and prompt-stratified values;
- nonparametric bootstrap 95% confidence interval for median tokens/s using
  fixed analysis seed `1750003` and 10,000 resamples;
- p95 inter-token interval from all measured warm intervals, plus per-invocation
  p95 to expose long-answer weighting;
- component attribution by median and p95 duration and fraction of TPOT.

No null-hypothesis significance test is required: qualification is against
registered engineering thresholds. The bootstrap interval describes uncertainty
and does not replace the literal verdict rule.

### Verdict

- `FAIL`: any mandatory correctness, security, provenance, or zero-fallback
  condition fails, including G6C conversation-residency qualification.
- `FUNCTIONAL_PASS_PERFORMANCE_MISS`: all correctness conditions pass but either
  the G5 cache-effectiveness control fails, median tokens/s <20.0, or p95
  inter-token >75 ms.
- `PERFORMANCE_PASS`: all correctness conditions, every G5 cache-effectiveness
  control, and both end-to-end thresholds pass.

## Reproducibility Classification

- Token/result/security/count outputs: deterministic; exact match required for
  the same candidate/workload.
- Timing/resource outputs: environment-sensitive; retain environment identity
  and distributions, never require byte-identical timing.
- Re-running with a different SIF, model digest, GPU class, driver, role plan, or
  workload produces a new experiment subject and cannot be pooled as a repeat.

## Stop and Escalation Conditions

- Stop before Tiger if any G0-G4 gate fails or any same-subject native exit is
  unclassified.
- Stop before model staging if the historical-control delta manifest is
  incomplete or the G4T current-SIF deployment control fails.
- Stop a Tiger sequence after the first failed preflight or mandatory functional
  case; do not launch the performance campaign.
- Preserve partial evidence for OOM, timeout, process exit, SIF mismatch, NFD
  failure, or protocol failure; do not silently resubmit.
- Reproduce a Tiger failure at the smallest applicable local gate and update the
  registered test before a new candidate is promoted.
- If G6 is functionally correct but misses performance, close the functional
  feature honestly and open a separate optimization Spec based on measured
  component attribution; do not change Spec 175 defaults post hoc.
