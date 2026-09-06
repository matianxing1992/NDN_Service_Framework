# T026 single-GPU four-Provider diagnostic (Job 207666)

Date: 2026-08-31

## Verdict

`DIAGNOSTIC_PASS`. Tiger job `207666` completed with Slurm state
`COMPLETED`, exit `0:0`, on `itiger07` using one
`NVIDIA RTX 5000 Ada Generation`. The job ran four independent NDNSF Provider
processes, one complete role per process:

| Provider identity | Role | ORT runner | GPU UUID | CPU model fallback |
|---|---|---|---|---|
| `/NDNSF-DI/Tracer/provider/backbone` | `/Backbone` | `onnxruntime-cuda` | `GPU-17a730b5-7e0d-99f3-b68b-83e283a14e46` | `false` |
| `/NDNSF-DI/Tracer/provider/head0` | `/Head/Shard/0` | `onnxruntime-cuda` | `GPU-17a730b5-7e0d-99f3-b68b-83e283a14e46` | `false` |
| `/NDNSF-DI/Tracer/provider/head1` | `/Head/Shard/1` | `onnxruntime-cuda` | `GPU-17a730b5-7e0d-99f3-b68b-83e283a14e46` | `false` |
| `/NDNSF-DI/Tracer/provider/merge` | `/Merge` | `onnxruntime-cuda` | `GPU-17a730b5-7e0d-99f3-b68b-83e283a14e46` | `false` |

The terminal validator recorded `user_rc=0`, `selection_ok=1`,
`dependency_ok=1`, and `response_ok=1`. The workload therefore completed the
real Request/ACK/Selection path, all four role executions and dependency edges,
and the final Response. The terminal marker was
`SPEC170_D0_CURRENT_NETWORK_PASS`.

The exact runtime was
`/project/tma1/ndnsf-di/releases/spec175-diagnostic-666d33c6/runtime.sif`,
SHA-256
`666d33c6b24964df594e8fc97b0e4e0a0c23a1a0443615baf965d010cb9b81ad`.
Job `207657` independently completed the same candidate with exit `0:0` before
the canonical repeat `207666`.

## Evidence boundary

This result proves that the current SIF can run four independent NDNSF Provider
processes on one allocated GPU, that all four ORT sessions execute model nodes
through `CUDAExecutionProvider`, and that the four-role NDNSF dataflow reaches a
final Response without CPU model fallback. It uses a minimal inspectable ONNX
pipeline fixture. It is not Qwen inference, does not exercise autoregressive
KV-cache/state continuity, and cannot close G5, G6, G6C, G7, T025, T026, T027,
or T034.

The retained compact evidence is under
`results/spec175/tiger/diagnostic-small-gpu-207666/`. Full remote logs remain
under
`/project/tma1/ndnsf-di/evidence/spec175-small-onnx-gpu-666d33c6/run-207666/`.
