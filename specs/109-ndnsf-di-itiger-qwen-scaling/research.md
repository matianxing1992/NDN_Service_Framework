# Research: NDNSF-DI iTiger Qwen Scaling

## Decision 1: Use Qwen2.5-Instruct as the controlled first family

**Rationale**: Current NDNSF-DI Qwen work is tied to Qwen2.5-0.5B-Instruct, and Qwen2.5 supplies a seven-size dense-model ladder: 0.5B, 1.5B, 3B, 7B, 14B, 32B, and 72B. Holding the family constant makes size the primary independent variable.
**Sources**: Qwen official release and model-card pages: <https://qwenlm.github.io/blog/qwen2.5/> and <https://qwenlm.github.io/blog/qwen2.5-llm/>.
**Alternatives**: Mixed Qwen2.5/Qwen3 matrix rejected; specialized coder/math/VL models rejected; both introduce architecture/task confounds.

## Decision 2: Separate standalone, artifact, and NDNSF-DI evidence

**Rationale**: Job 145855 proves only Slurm/GPU/Apptainer/scratch substrate. A standalone Transformers run proves complete-model capacity and tokens. Export validation proves model-artifact equivalence. Only the real request/security/provider path proves NDNSF-DI.
**Alternatives**: Treat GPU visibility or standalone inference as candidate PASS rejected as an authority error.

## Decision 3: Store durable data in project and transient amplification in allocation scratch

**Rationale**: The account email allocates small `/home`, project storage for code/data, and large temporary job storage. Model sources and accepted artifacts need durable project identity; conversion amplification and runtime caches are disposable.
**Alternatives**: Local workstation rejected due capacity; `/home` rejected by quota/policy; scratch-only rejected because results may disappear.

## Decision 4: Stage models sequentially with explicit peak projection

**Rationale**: FP16/BF16 weights alone are approximately two bytes per parameter. Source, export, external data, cache, and evidence can coexist during conversion, so a project quota near 200 GB cannot safely retain the complete source/export matrix and is unlikely to admit 72B. Actual file manifests replace estimates before transfer.
**Alternatives**: Download all models rejected; automatic deletion rejected because accepted/referenced artifacts require protection.

## Decision 5: Start one-node/multi-GPU before multi-node

**Rationale**: iTiger multi-node NFD addressability/TCP/UDP is unverified. One-node allocations avoid cross-node network uncertainty while still testing GPU-stage placement.
**Alternatives**: Immediate multi-node rejected until the Spec 108 network probe is admissible.

## Decision 6: Use GPU classes as hypotheses, not hard-coded truth

**Rationale**: Preliminary evidence observed RTX 5000 on itiger07-11, RTX 6000 on itiger02-06, and H100 on itiger01, but scheduler configuration is mutable. Profiles name GRES, while preflight records the actual node/GPU.
**Alternatives**: Physical GPU indices rejected because Slurm/container mappings differ.

## Decision 7: Lock an FP16 first matrix and fail closed on backend fallback

**Rationale**: Quantization changes model behavior, memory, compute, and export tooling. The first scale study should not mix it with size. ONNX Runtime CUDA evidence must be observed for every stage. Official ONNX Runtime documentation requires compatible CUDA/cuDNN user-space and host drivers: <https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html>.
**Alternatives**: Silent CPU fallback rejected; 4/8-bit fallback rejected as a replacement result.

## Decision 8: Use exact tokens before performance

**Rationale**: Distributed generation, KV state, stage boundaries, and tensor dtype can produce plausible but wrong text. Greedy input/output token IDs allow exact equivalence for 1/2/32-token cells.
**Alternatives**: String similarity or perplexity-only validation rejected.

## Decision 9: Three original 60-second repetitions

**Rationale**: The repository requires a 60-second short performance window. Independent repetitions expose scheduler/cache variability and retain failure denominators. Warmup stays outside measurement.
**Alternatives**: One measurement rejected as fragile; pooled rescue rejected; automatic retry rejected.

## Decision 10: Create Spec 109 rather than extending Specs 107/108

**Rationale**: Spec 107 owns local MiniNDN candidate/runtime recovery; Spec 108 owns portable deployment adapters; Spec 109 owns model-scale experimental policy and evidence. This prevents packaging from absorbing research-specific model ladders and statistical gates.
**Alternatives**: Appending the matrix to Spec 108 rejected due ownership and already broad scope.

## Decision 11: Use separate correctness and performance references

**Rationale**: Full-model Transformers/PyTorch is an independent token/capacity oracle, but its graph, runtime, topology, and communication path differ from a staged NDNSF-DI candidate. Performance overhead is therefore computed only against a staged ONNX Runtime baseline using the candidate's exact artifacts, session options, GPU mapping, workload, cache state, warmup, and logging.
**Alternatives**: Transformers timing as the overhead denominator rejected as structurally confounded; candidate-only latency rejected because it cannot isolate NDNSF cost.

## Decision 12: Split descriptive scaling from controlled size-effect analysis

**Rationale**: Changing model size together with GPU class/count and placement prevents a causal model-size claim. The full ladder reports an operational envelope; only sizes sharing one hardware/resource/workload block enter the controlled size-effect subset.
**Alternatives**: Regressing all seven sizes against parameter count rejected because hardware and placement would be unmodeled confounders.

## Decision 13: Gate percentiles and reproduction by sample support

**Rationale**: Three 60-second repetitions preserve the repository's short-test rule, but low-throughput models cannot support stable tail percentiles. p50/p95/p99 require 20/100/1000 completed observations respectively; unavailable tails remain explicit. Reproduction uses confidence intervals around a preregistered engineering margin rather than a naked point-estimate threshold.
**Alternatives**: Reporting interpolated p99 from tens of requests rejected; pooling repetitions to reach a denominator rejected because it hides failed or heterogeneous runs.

## Decision 14: Require node-level ONNX Runtime execution-provider evidence

**Rationale**: Provider registration and session creation do not prove that every graph node executed on CUDA; unsupported nodes may be assigned to CPU. Each role must retain profiling/assignment evidence and correlate CUDA work to the allocated GPU UUID.
**Source**: ONNX Runtime execution-provider priority and CUDA provider documentation: <https://onnxruntime.ai/docs/execution-providers/> and <https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html>.
**Alternatives**: `CUDAExecutionProvider` present in the provider list rejected as GPU PASS evidence.

## Decision 15: Seal source, exact predecessors, and scoped gates

**Rationale**: The current repository can contain uncommitted and untracked experiment code, while a spec-level dependency label cannot prove which behavior exists. Campaign identity therefore includes a reconstructable source snapshot and an exact task/status/artifact/digest predecessor manifest. Gate scope is systemic, model-local, or placement-local so one model's license/fit failure does not censor unrelated admissible sizes.
**Alternatives**: Bare `HEAD`, task ranges, and a single global ladder stop rejected as non-reconstructable or over-broad.

## Decision 16: Compose Spec 108 instead of copying deployment resources

**Rationale**: Account, QOS, CPU, memory, walltime, GRES, image, and release behavior are deployment concerns already owned by Spec 108. Spec 109 references the deployment profile/release digest and adds only model/workload/stage bindings. Repository-local automation is canonical; the personal iTiger Skill is an optional operator wrapper.
**Alternatives**: A second Spec 109 resource profile was rejected as configuration drift; absolute personal-tool paths were rejected as nonportable execution dependencies.

## Current verified code facts

- `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` defaults to `Qwen/Qwen2.5-0.5B-Instruct` and native bundle generation hard-codes that model and a three-stage plan.
- `OnnxRuntimeModelRunner` supports CPU/CUDA provider selection and fallback policy, but its current evidence records the selected provider rather than complete graph-node assignment; accepted iTiger candidate evidence has not run.
- `Experiments/NDNSF_DI_QwenFull_OnnxVsTransformers_LocalBenchmark.py` already contains `run_matched_staged_baseline`, which is a reusable starting point but currently constructs CPU ONNX Runtime sessions and lacks the Spec 109 workload/GPU/assignment contract.
- Spec 108 offline Slurm/Apptainer tests passed 60/60 with zero live adapter submissions.
- Preliminary job 145855 completed on an RTX 5000 and proved only substrate capability.

## Unverified external facts to rediscover

- actual project quota and enforcement mechanism;
- current partition/account/QOS/GRES/node state;
- current Apptainer versions and compute-node egress;
- wait-time and maximum requested GPU policy;
- current model repository revisions, file sizes, license text, and gating;
- whether 32B/72B export peak fits after quota expansion.
