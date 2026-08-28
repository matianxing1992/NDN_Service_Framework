# TigerCluster Gate D2a Evidence

Status: **PASS**  
Canonical job: `200959`  
Date: 2026-08-18  
Node: `itiger01`

## Frozen inputs

- SIF: `spec170-runtime-r19-d2-v7-20260818/runtime.sif`
- SIF SHA-256: `0556a37593aa26b04271b2e01f12089fa6047a873fd03fea91142dce5506a479`
- Bundle: `spec170-runtime-r19-d2-v7-20260818/d2-bundles-v7/d2a`
- Slurm allocation: one node, 12 CPUs, 48 GiB, and two `h100_80gb` GPUs
- Workload: one NDNSF Provider with two independent CUDA roles and one
  Provider-local two-rank NCCL group

The release's `runtime.sif` is an immutable link to the exact r19 SIF. The
compute-node stage resolved the link, copied 4,396,982,272 bytes, and verified
the canonical SHA-256 before starting the workload.

## Positive path

The Provider required exactly two visible CUDA devices, disabled ONNX Runtime
CPU fallback, and selected `CUDAExecutionProvider` on each device. The retained
markers show:

- a positive runtime ACK for `cuda:0,cuda:1`;
- one committed Selection binding all four roles to the Provider;
- completion of `/D2A/Independent/0` on `cuda:0`;
- completion of `/D2A/Independent/1` on `cuda:1`;
- completion of `/D2A/LocalGroup#0` and `/D2A/LocalGroup#1` through the real
  PyTorch NCCL collective;
- one complete final Response, with no CPU fallback.

The workload terminal marker is:

```text
SPEC170_D2A_NETWORK_PASS job=200959 independentRoles=2 localRanks=2 collective=nccl groupFailure=PASS
```

Slurm finished the batch with `COMPLETED`, exit code `0:0`, after 50 seconds.

## Whole-group failure path

The second request intentionally removed local rank 1 after ACK and Selection.
The Provider completed the two independent roles and local rank 0, recorded
`D2A_LOCAL_RANK_1_LOST`, and published no partial final Response. The User
observed the bounded failure and emitted `SPEC170_D2A_GROUP_FAILURE_PASS`.

## Diagnostic lineage and regression shield

- Job `200952` exposed a stale Python binding that omitted the native
  `roleProviders` map. The r19 extension exposes and preserves that map.
- Job `200956` reached ACK and Selection but completed only the first role.
  Core correctly groups all roles assigned to one Provider into one Selection
  assignment set and invokes the Provider handler once; the D2a workload had
  incorrectly expected one callback per role. The workload now executes the
  entire Provider-scoped assignment set exactly once.
- `tests/python/test_spec170_tiger_workloads.py` now fixes that one-callback,
  all-local-roles contract and the no-partial-group contract.

Raw evidence is retained at:

```text
/project/tma1/ndnsf-di/evidence/spec170/d2a-local-two-gpu-200959
```

