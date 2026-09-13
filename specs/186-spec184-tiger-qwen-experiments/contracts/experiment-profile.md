# Spec186 Experiment Profile Contract

## Profile Schema

Profile schema identifier: `ndnsf-spec186-experiment-profile-v1`.

Required top-level fields:

```text
schemaVersion
candidate
case
topology
roles
runtime
model
workload
security
timeouts
resources
evidence
```

Unknown or unconsumed fields MUST be rejected. Paths MUST be absolute after resolution,
remain under their declared root, and carry a digest when they are candidate inputs.

## Case Values

```text
yolo-minindn-normal
yolo-minindn-negative
qwen06b-minindn-cpu
yolo-tiger-single-gpu
yolo-tiger-two-node-normal
yolo-tiger-two-node-negative
yolo-tiger-two-node-reuse
qwen06b-tiger-experimental
```

`qwen06b-tiger-experimental` is conditional. It may run only when the model format,
backend, stage manifest, tokenizer and cluster resource declaration are complete.

## Role And Topology Rules

YOLO roles are exactly `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`.
Each role has an identity, node, optional GPU device, binary, service name and expected
backend. A two-node profile MUST declare two distinct physical hosts and one NFD endpoint
per host. A local MiniNDN profile MUST declare per-process HOME/PIB/TPM and socket paths.

Qwen roles use `/LLM/Pipeline/Stage/<n>` and MUST include stage order, artifact digest,
tokenizer digest, model URI and dependency names. The profile MUST state whether the
model is ONNX CPU, ONNX CUDA, GGUF/Q3 or another backend; no implicit conversion is
allowed.

## Runtime And Resource Rules

- `runtime.apptainer.path` and `runtime.apptainer.version` MUST be explicit for
  every profile; the version MUST be exactly `1.5.3`. Local MiniNDN/SIF paths
  MUST probe that executable with `--version`. Tiger profiles use
  `/usr/bin/apptainer` on allocated compute nodes; the login node is a
  submit/metadata boundary and its 1.3.4 package MUST NOT be used for SIF.
- `allowCpuFallback` MUST be explicit for every role; GPU qualification requires `false`.
- CUDA roles MUST include expected device identity and a signed `free_memory_mb` threshold.
- `cleanupSeconds`, startup, request and completion budgets MUST be finite.
- Negative completion budget starts only after all ranks publish the shared readiness barrier.
- Slurm account, partition, node constraints, GPU type and memory are recorded as effective
  values, not silently inferred from a login host.

## Effective Configuration Record

The launcher MUST write the fully resolved argv, environment allowlist, binds, node/GPU
map, NFD endpoints, model paths, candidate digest and timeout values before any remote
mutation. Effective configuration hash is part of the candidate tuple.
