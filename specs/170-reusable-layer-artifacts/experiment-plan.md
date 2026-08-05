# Experiment Plan: Spec 170 Correctness and Deployment Fidelity

## Material Passport

```yaml
schema: ARS-9
material_id: ndnsf-di-spec170-experiment-plan-v1
type: code_experiment_plan
status: PLANNED
created: 2026-08-04
data_status: no experiments executed by this plan
source_spec: specs/170-reusable-layer-artifacts/spec.md
implementation_plan: specs/170-reusable-layer-artifacts/plan.md
```

## Research Question

Can NDNSF-DI reuse one canonical model-layer representation while safely and
correctly planning/executing CPU, single-device, multiple independent-device,
multi-rank, and heterogeneous pipeline/tensor placements from truthful signed
Provider offers under the same authenticated request lifecycle?

## Hypotheses and Blocking Criteria

### H1 - Resource truth

The signed Provider offer equals the CPU/accelerator set actually visible inside
the container, and configuration only removes resources.

Block if any phantom/unallocated device is offered/selected, any visible device
outside the configured subset is selected, or the offer/profile/snapshot cannot
be bound to the final Selection.

### H2 - CPU and one-GPU compatibility

CPU-allowed/no-GPU and one-GPU profiles complete the same logical workload as a
matching unsplit backend/dtype oracle. GPU-required requests never silently use
CPU.

Block on any deterministic token mismatch, tensor/logit tolerance violation,
silent fallback, request-ID discontinuity, security bypass, or incomplete
Response.

### H3 - Multiple independent devices under one Provider

One Provider safely maps independent logical roles/requests to different GPUs
with per-device accounting.

Block if `2 x 12 GiB` satisfies one unsplittable 20-GiB role, if two individually
feasible roles overlap capacity, if one device's runtime/cache aliases another,
if ACK creates any reservation/queue/resource hold, or if Selection queue
acceptance acquires device capacity before just-in-time admission.

### H4 - One logical role across devices

A device-set role is feasible only with complete adapter-certified ranks,
partition recipes, topology, collectives, and atomic admission.

Two distinct variants are tested: (a) every rank/device is inside one
Provider-local bundle and one local admission transaction; (b) ranks span
multiple Provider-local bundles with authenticated cross-Provider rendezvous,
independent local admission, and whole-epoch failure propagation. Evidence from
one variant cannot satisfy the other.

Block on partial member admission/hold, incomplete rank cover, unsupported link
or layout acceptance, collective ordering/epoch error, deadlock, silent member
replacement, non-group failure propagation, or any cross-Provider payload that
bypasses the `NDNSF_DATA_V1` capability/manifest/segment contract.

### H5 - Heterogeneous hybrid correctness

Plans use `N x {M_i}`. `[1,2,1]` and `[2,1,2]` match an unsplit oracle while only
the intended stages have ranks/collectives and only unequal/incompatible
boundaries have explicit redistribution.

Block if stage rank count differs from `M_i`, an `M_i=1` stage has a phantom
tensor rank/collective, a required redistribution is absent/duplicated, or the
complete output violates the frozen oracle/tolerance.

### H6 - Data-driven liveness

An ordinary stage starts after its own local-ready plus authenticated predecessor
data; a tensor stage starts after its complete local/remote group plus input. No
role waits for unrelated stages or a second start command.

Block on any global model-ready barrier, unrelated-stage wait, second execution
command, early consumption of incomplete group/boundary data, or trace invariant
violation.

### H7 - Exact reuse

Changing pipeline boundaries, degree vector, or Provider placement preserves
canonical model/layer identities. An exact loaded-runtime warm hit has zero Repo
model bytes, zero fragment build, and zero model reload. A Provider that refuses
new preparation remains selectable through `ACCEPT_IF_EXACT_REUSE` only when
the sealer verifies the exact proposed role/rank assembly/runtime identity.

Block on duplicate canonical publication, false loaded-runtime hit after device/
topology/boot/runtime change, nonzero model preparation for an exact live hit,
selection of an overall negative ACK, or cold/canonical-only selection of a
reuse-only offer.

### H8 - Failure observability

Every injected failure retains request ID/attempt/plan, precise lifecycle class,
and last verified progress checkpoint.

Block if a known boundary collapses into generic-timeout-only evidence or an
unsafe partial artifact/runtime is advertised as reusable.

### H9 - Exact-SIF parity

One sealed OCI/SIF identity exhibits consistent source/contracts and truthful
0/1/2-GPU visibility under Slurm/Apptainer.

Block if local/container/remote identities differ, unallocated GPUs appear, a
one-Provider/two-GPU conclusion is produced from two single-GPU Providers, or a
cross-Provider conclusion is produced without two independently bound Provider
offers/bundles and authenticated rendezvous evidence.
The candidate is frozen only after executable source, security, build/install,
harness, model/artifact preparation, and Gate A/B/C closure. Any post-freeze
hash mismatch is `INVALID_CANDIDATE`.

### H10 - Custom strategy containment and sealing

An operator-installed custom strategy receives only sanitized immutable
planning views and produces a declarative proposal. The trusted sealer rejects
every invalid mutation, and timeout/exception/cancellation/budget exhaustion
publishes no Selection.

Block if raw wire/proof/runtime objects reach the public strategy input, request
or network content can load strategy code, a failed strategy publishes
Selection, grants depend circularly on final `planDigest`, incomplete/substituted
grant cover finalizes, or semantically equivalent valid proposals seal to
different core/final plan identities.

## Independent Variables

- accelerator exposure: 0, 1, or 2 devices;
- Provider policy: `AUTO`, `NONE`, `EXPLICIT_SUBSET`;
- placement profile: V2 single-device compatibility or V3;
- placement strategy: built-in or operator-installed custom implementation;
- role topology: pipeline-only, tensor-only, heterogeneous hybrid;
- per-stage degree vector: `[1,1,1]`, `[2,2,2]`, `[1,2,1]`, `[2,1,2]`;
- tensor distribution: sharded, replicated, owner-only, local-derived;
- device topology: peer capable/incapable, symmetric/asymmetric memory,
  exclusive/unsupported sharing or failure domains;
- cache state: cold, canonical-only, assembled-fragment, exact loaded runtime;
- offer disposition: exact-reuse-only, preparation-accepted, or reject;
- concurrency and resource contention;
- injected lifecycle/resource/tensor/network fault.

## Dependent Variables

- complete output/token and intermediate tensor/logit equivalence;
- rank, collective, redistribution, and dependency trace invariants;
- accepted/rejected placement and admission outcome;
- request/attempt/plan continuity and exact failure classification;
- Repo model bytes, duplicate bytes, fetch/build/load counts;
- planning, queue, transfer, assembly, load, collective, TTFT, per-token, and
  total latency;
- tokens/s and per-device peak memory/utilization;
- cold/warm residency level and cache-hit truth;
- deadlocks, partial holds, timeouts, retries, and CPU fallback count.

## Controls

- Freeze model/weights, tokenizer, adapter/recipe, backend, dtype, generation
  options, seed, prompt, source/SIF, security policy, routes, and repository
  payload for paired comparisons.
- Compare distributed output with an unsplit oracle using the same backend/dtype;
  do not misclassify ordinary CPU/GPU numerical differences as partition error.
- Use `[1,1,1]` as the pipeline structural control and `[2,2,2]` as the uniform
  tensor-degree control.
- Use an adapter lacking the requested transition/collective as a negative control.
- Keep cold and warm samples separate. Keep clean-network correctness separate
  from delay/loss/fault campaigns.
- Strategy acceptance proves feasibility, safety, determinism, and evidence; it
  does not claim global optimization without a separate baseline study.

## Reference Cases

Maintain at least 20 immutable oracle-labelled cases. The real-prompt corpus is
one versioned file with eight IDs:

- `P01-P05` are the locked primary performance subset spanning short/medium/long
  input and output classes;
- `P06-P08` are validation-only prompts for Unicode/tokenization, long-context,
  and EOS/termination behavior and are excluded from performance aggregates;
- four boundary tensor shapes/layouts;
- four cold/warm/identity variants;
- four adversarial topology/contract cases.

Adapter partition/redistribution recipes receive one human review when first
introduced. Runtime correctness uses deterministic code/oracles, not an LLM
judge.

## Validation Ladder

### P0 - Contract and property tests

- Enumerate `N=1..4` and `M_i=1..3` within a bounded 120-vector deterministic
  corpus.
- Generate invalid mutations: `M_i=0`, missing/duplicate/orphan ranks, illegal
  axis/layout, incomplete tensor distribution, unsupported transition,
  unoffered handle, resource overflow, and cycle.
- Require all valid cases to seal deterministically and all invalid mutations to
  be rejected at their intended boundary.
- Use a tiny deterministic ONNX graph/fake runner. This proves contracts/graph
  coverage, not CUDA/NCCL.

### P1 - CPU/no-GPU and one-device compatibility

- Run real MiniNDN with the minimal real model on the 8-GiB local host.
- Start three independent Provider processes and issue three concurrent V3
  invocations through the unmodified normal Application default. The locked
  memory-safe profile uses one three-stage CPU pipeline: `P0`, `P1`, and `P2`
  each own one stage fragment and every invocation selects all three Providers.
  Three requests therefore reuse/single-flight the same per-Provider fragment
  rather than loading three complete models. A single Provider with simulated
  devices is a separate scheduler test, not this gate.
- Exercise CPU-allowed success and GPU-required/no-GPU rejection.
- Exercise the one-device planner contract with simulated topology; add local
  CUDA only if actual hardware exists and label it accurately.
- Verify security, request ID, canonical publication, Provider-local assembly,
  data-driven start, full multi-token Response, and cold/warm reuse.

### P2 - One Provider, two independent roles

- Use a simulated two-device topology plus real MiniNDN control/data paths.
- Bind role A to device 0 and role B to device 1; vary concurrency and queueing.
- Test asymmetric envelopes and the `2 x 12 GiB` versus 20-GiB trap.
- Actual dual-GPU CUDA evidence remains a TigerCluster requirement.

### P3 - One logical role, two ranks/devices

- Use a deterministic tensor adapter and collective emulator locally.
- Verify complete rank/tensor coverage, atomic group admission, operation order,
  timeout/cancellation, and rank-loss propagation.
- Run a Provider-local variant with one two-device bundle and a cross-Provider
  variant with two one-device bundles; compare both with the same unsplit oracle
  while retaining distinct admission/rendezvous traces.
- The cross-Provider variant uses `NDNSF_DATA_V1` and exercises signed operation
  manifests, HMAC-bound segments, bounded inflight state, exact duplicate versus
  conflicting replay, no-progress cancellation, and zero partial output.
- Actual NCCL/P2P behavior remains a TigerCluster requirement.

### P4 - Heterogeneous hybrid

- Positive controls: `[1,1,1]` and `[2,2,2]`.
- Main profiles: `[1,2,1]` and `[2,1,2]` with the same model, Providers, input,
  and oracle.
- Verify collectives only where `M_i>1`; verify redistribution only where degree
  or tensor layout requires it; verify unsplit stages have no phantom ranks.
- Verify canonical model/layer identities remain unchanged across all vectors.

### P5 - TigerCluster exact-SIF

P0-P4, the real three-Provider MiniNDN repeated-request gate, protected/security
mutations, and exact-SIF CPU/native parity must pass before the sole T029 freeze.
P5 executes only that frozen candidate. Any source/build/job/model/workload hash
mismatch is `INVALID_CANDIDATE`, not a remote diagnostic retry.

Run separate immutable Slurm allocation blocks:

- **0 GPU**: no GRES/GPU request and no GPU exposure; capture a zero-device offer
  and CPU-allowed result or GPU-required rejection.
- **1 GPU**: request one GPU, use Apptainer `--nv`, and require one exact device
  in probe/offer/Selection plus complete small-model GPU output.
- **2 GPUs, D2a**: request two GPUs for one Provider task, use `--nv`, and
  require both and only allocated devices. Run independent roles and one
  Provider-local two-rank role.
- **2 GPUs, D2b**: use a separately declared allocation/topology with two
  Provider runtimes restricted to one allocated GPU each. Run one two-rank
  cross-Provider role and require two local bundles plus authenticated
  rendezvous/transport/epoch evidence.
- **2 GPUs, D2h**: after D2a/D2b, run `[1,2,1]` and `[2,1,2]` as separate
  heterogeneous-hybrid profiles on this frozen mapping:
  `[1,2,1]`: `P0/G0={S0R0,S1R0}`, `P1/G1={S1R1,S2R0}`;
  `[2,1,2]`: `P0/G0={S0R0,S1R0,S2R0}`, `P1/G1={S0R1,S2R1}`. Co-resident ranks
  use one `EXCLUSIVE_PLAN` local admission vector with summed phase peaks. Retain
  per-stage ranks, collectives, redistribution, activation, oracle, and failure
  rows. Insufficient resources produce `BLOCK`, not an inferred hardware claim.

Retain scheduler allocation variables, `CUDA_VISIBLE_DEVICES`, `nvidia-smi`,
runtime device count/UUID map, signed topology/profile/snapshot, Selection
binding, and per-rank/device evidence. If TigerCluster cannot allocate two GPUs
to one task/container, D2a is `BLOCK`; two single-GPU Providers are not a
substitute for D2a, but are the intentionally separate topology required by
D2b. Conversely, D2a does not establish cross-Provider execution.
D2h is also an independent correctness claim and cannot be inferred by joining
the D2a and D2b summaries.

## Fault Injection Matrix

### Strategy and sealing

- custom strategy timeout, exception, cancellation, or candidate-budget
  exhaustion;
- proposal selects an unoffered device or stale offer/view;
- incomplete rank/tensor cover or incompatible degree/layout transition;
- opaque executable/runtime object embedded in a proposal;
- attempt to load strategy code from request/Repo/network content;
- reorder semantically identical proposal entries before deterministic sealing.

### Resource and admission

- hide, renumber, lose, or degrade a device after ACK;
- stale/replayed resource sequence or mismatched topology/profile digest;
- MIG/parent/failure-domain change;
- explicit configuration references an absent device;
- Selection uses an unoffered handle;
- two requests contend for one device;
- only part of a device set is available;
- weights fit but activation/KV/collective/transient peak does not.

### Tensor, collective, and hybrid boundaries

- missing/duplicate rank or wrong world size;
- wrong shard/replication/owner rule, layout, padding, or operation order;
- peer access/collective capability absent;
- communicator/group epoch mismatch, rank crash, hang, or corrupt shard;
- omit or duplicate `1<->2` redistribution;
- wrong producer/consumer rank map;
- delay one rank and attempt downstream early execution;
- duplicate, stale, or orphan boundary output.

### Artifact, reuse, lifecycle, and network

- corrupt canonical chunk or incomplete root manifest;
- wrong model/profile/adapter/assembly digest;
- claim a loaded-runtime hit after device-set change;
- Repo fetch continues progress beyond a former fixed wall timeout;
- Repo progress stops and triggers no-progress deadline;
- replay/mismatch request ID, attempt, plan, group epoch, or Response.
- protected-profile wrong-recipient/stale/replayed `KeyGrantV1`, revocation or
  protection-epoch advance during host/device residency, incomplete plaintext
  registry, zeroization failure, and an old runtime advertised after revocation;
- cross-Provider wrong capability/peer/epoch key, oversized operation manifest,
  conflicting segment replay, segment completion before full bitmap, operation
  reordering, and late Data after cancellation.

Every injection uses a fixed seed/trigger point and expected lifecycle class.

## Repetitions and Statistical Reporting

### Correctness and race repetitions

- Deterministic contract/property mutation runs at least once per frozen case;
  correctness requires zero unexpected outcomes.
- Admission/rank delay/loss races use 50 fixed, pre-recorded scheduling seeds per
  fault class and require zero invariant violations or deadlocks.
- Classification results report exact numerator/denominator and two-sided
  Clopper-Pearson 95% intervals. Observing 100% does not justify a universal
  zero-failure claim.

### Cold/warm sampling unit

Publication-quality performance requires exactly three complete clean-start
blocks. Fewer blocks are retained as `EXPLORATORY` and cannot close SC-034. For
each configuration, start block, and primary prompt `P01-P05`, execute:

```text
1. verify process/container start identity and reset this model/profile's
   canonical, assembled-fragment, and loaded-runtime Provider caches;
2. issue one measured COLD request (coldSample=true);
3. issue one unmeasured WARMUP request after the cold request completes;
4. issue five measured WARM requests without eviction or process restart.
```

Thus each configuration has 15 measured cold requests and 75 measured warm
requests. The cold request is never discarded as warmup. Cache reset is scoped
to the tested model/profile and records pre/post inventory proof; it does not
rebuild the candidate, change model bytes, or alter routes/security/config.

### Estimands and intervals

- Primary correctness estimand: complete authenticated response rate with oracle
  match; acceptance requires all deterministic rows to pass.
- Primary performance estimand: paired warm total-latency ratio
  `treatment/baseline` at identical `(start, prompt, repetition)`; report median
  ratio and paired median latency difference. Secondary estimands are TTFT,
  tokens/s, Repo bytes, assembly/load count, and p95 total latency.
- Effect size is the paired median log latency ratio, back-transformed to a
  ratio, plus the paired median difference in milliseconds.
- Confidence intervals use a deterministic hierarchical bootstrap with 10,000
  iterations and recorded seed: resample clean-start blocks first, prompts within
  each sampled block second, and paired repetitions within prompt third. Never
  bootstrap individual requests as independent across starts/prompts.
- The warm-latency equivalence test uses TOST with two one-sided tests at
  `alpha=0.05` (equivalently, the 90% CI) on the paired ratio and the predeclared
  interval `[1/1.10, 1.10]`. The p95 ratio threshold `<=1.15` is a secondary
  descriptive guard, not a second equivalence test.
- The Holm family consists of the primary warm-latency comparisons of each V3
  accepted topology (`CPU`, `SINGLE_DEVICE`, independent roles, 3A, 3B, 3C)
  against its frozen matched control. Apply Holm only within that family; report
  unadjusted and adjusted p-values, effect size, and interval together.
- Report cold and warm median, IQR, p95, hierarchical-bootstrap 95% CI, and raw
  row counts separately. Do not pool cold and warm observations.

Pre-register numerical thresholds before execution:

- deterministic greedy token IDs: 100% identical;
- FP32 hidden/logits: suggested `atol=1e-5`, `rtol=1e-4`;
- FP16/BF16 hidden/logits: suggested `atol=2e-2`, `rtol=2e-2`, cosine >=0.999;
- adapter/backend may specify stricter justified thresholds, but thresholds may
  not be loosened after results are observed.

Small-model multi-GPU speedup is not a correctness gate; collective overhead may
make it slower. For the existing one-GPU compatibility path, investigate/block
if paired warm median latency ratio exceeds 1.10 or p95 exceeds 1.15 and the
interval does not support equivalence.

## Online Guardrails

For every request:

- verify offer/profile/snapshot/resource sequence and exact device handles;
- validate rank/tensor/graph complete cover and resource envelopes;
- forbid silent device/CPU remap and cross-attempt state;
- verify ACK created no reservation/queue entry, Selection queue acceptance held
  no device capacity, and just-in-time device-set admission was atomic with a
  current fencing token;
- bind all data/status/Response to request, attempt, plan, role/rank/group epoch;
- fail closed at the narrowest boundary.

## Evidence and Retention

Retain only canonical reproduction runs and the latest distinct diagnostic case.
Each formal bundle binds source/OCI/SIF/model/artifact/profile/prompt/security/
route/schedule identities, full result rows, lifecycle traces, resource samples,
and failure classification. Never replace negative rows with a later successful
run or infer large-model readiness from this minimal-model qualification.
