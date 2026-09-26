# Qwen scale experiments on iTiger

## Scope

Use this reference when planning, staging, executing, or validating Qwen model
sizes for NDNSF-DI. Start with the Qwen2.5-Instruct family because the current
NDNSF-DI pipeline already uses Qwen2.5-0.5B-Instruct. Treat later Qwen families
as separate experiment identities.

## Durable layout

```text
/project/$USER/ndnsf-di/
  src/<git-revision>/
  images/<oci-digest>/<release>.sif
  models/hf/<model>/<revision>/
  models/onnx/<model>/<export-digest>/stage-<n>/
  cache/huggingface/
  cache/apptainer/
  manifests/
  evidence/<campaign-id>/<run-id>/

/tmp/$USER/ndnsf-di/$SLURM_JOB_ID/
  download-staging/
  export-staging/
  runtime/
  evidence-staging/
```

Set `HF_HOME` and `APPTAINER_CACHEDIR` under project storage. Set `TMPDIR` and
`APPTAINER_TMPDIR` under the allocation's `/tmp`. Promote required evidence to
project storage before exit; scratch cleanup is not evidence promotion.

## Initial model ladder

| Qwen2.5 size | First standalone GPU | First NDNSF-DI placement | Planning note |
|---|---|---|---|
| 0.5B | 1 RTX 5000 | one node, up to 3 GPUs/stages | MVP and exact-token oracle |
| 1.5B | 1 RTX 5000 | one node, up to 3 GPUs/stages | advance only after 0.5B |
| 3B | 1 RTX 5000 | one node, up to 3 GPUs/stages | record non-Apache license |
| 7B | 1 RTX 5000 | one node, 3 GPUs/stages | first meaningful transfer load |
| 14B | 1 RTX 6000 or H100 | one node, 3 GPUs/stages | avoid marginal 32 GiB fit |
| 32B | 1 H100 reference | one node, 3-4 GPUs/stages | require projected peak gate |
| 72B | at least 2 H100 reference | one node first, 3-4 H100 stages | require quota expansion |

GPU assignments are starting hypotheses, not permanent cluster facts. Preflight
must rediscover GRES and record actual allocation and GPU UUIDs.

## Experiment identity

Bind every cell to:

```text
model repository + immutable revision + license record
tokenizer digest + prompt-set digest
source revision + candidate ID
OCI digest + SIF SHA-256
dtype + quantization (normally none in the first matrix)
exporter/version/opset + stage partition map + artifact digests
GPU GRES/count + node count + CPU/memory/walltime
context length + maximum generated tokens + sampling configuration
run ID + repetition ID
```

Changing any bound field creates a new candidate/cell. It never replaces a
failed measured repetition.

## Measurement pyramid

1. `development-smoke`: one short run; debug identity; never acceptance.
2. `standalone-reference`: greedy decode, fixed prompt set, exact token record.
3. `artifact-correctness`: full model versus exported/staged model, 1/2/32
   generated tokens, exact equality.
4. `ndnsf-di-correctness`: normal network/security path and exact final tokens.
5. `ndnsf-di-performance`: one excluded warmup followed by three independent
   60-second measured repetitions.
6. `scale-extension`: advance to the next model only after capacity, artifact,
   correctness, backend, cleanup, and evidence gates pass.

## Metrics

Capture completion/failure, exact-token equality, model and provider startup,
TTFT, inter-token latency, tokens/s, request throughput, p50/p95/p99, CPU RSS,
GPU memory/utilization/UUID, ONNX Runtime provider/fallback, per-stage compute,
queue and dependency wait, NDN bytes/segments, scratch peak, durable bytes, job
state/exit, and evidence-promotion result.

For distributed Qwen preparation, also retain phase-separated markers and
committed byte/range progress:

```text
publication -> catalog ACTIVE -> provider fetch/cache
-> residency/load -> collaboration -> stage execution -> response
```

Do not infer progress from a preallocated file size. A partial stage with a
stable sidecar range is a stalled-fetch diagnostic, not a successful model
transfer. Record the provider, artifact digest, range, last committed bytes,
retry count, and deadline state.

On TigerCluster, keep DKEY/control traffic and large artifact data on explicit
route classes. Install a deterministic `best-route` strategy for the DKEY
prefix and for the exact `.../provider/NDNSF-ARTIFACT` child prefix when the
parent inference namespace is multicast. Freeze these commands in the source
bundle; a live route repair is not a reproducible acceptance run.

The 2026-08-02 Qwen3.6-27B diagnostics (Slurm 181860 and 181866) registered
all three stages and reached partial Stage 0 CUDA execution, but neither
produced a complete response. They remain diagnostic negative evidence. Before
another large-model allocation, pass the same route graph and one deliberately
stalled-range case in MiniNDN, then complete a one-request multi-token smoke
with the existing Qwen3-0.6B manifest. Only after that should a formal
warmup-plus-five multi-request campaign be scheduled.

Compare standalone and NDNSF-DI only when model revision, dtype, prompt, context,
output length, GPU class/count, warmup, and measurement window match. Report
absolute results and overhead; do not call a capacity failure a performance
regression.

## Stop rules

- Stop before allocation on insufficient verified quota or projected reserve.
- Stop a model tier on checksum, license, export, exact-token, security,
  backend, cleanup, or evidence-promotion failure.
- Preserve OOM, scheduler, timeout, conversion, and network failures.
- Never silently retry a measured cell, change GPU type/count, shorten output,
  quantize, or delete failure evidence.
- Quantized, tensor-parallel, pipeline-parallel, and multi-node variants are new
  cells with separate baselines.
- Keep physical-production authority deferred to the physical deployment spec.
