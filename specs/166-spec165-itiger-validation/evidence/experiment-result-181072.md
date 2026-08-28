## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: UNVERIFIED
- Version Label: exp_result_v1

## Experiment Result

- **ID**: spec166-standalone-qwen3-003
- **Type**: generic
- **Status**: crashed
- **Command**: `sbatch` of immutable
  `/project/tma1/ndnsf-di/jobs/spec166/source-003/standalone-gpu-reference.sbatch`
  with submission/run ID `spec166-standalone-qwen3-003`
- **Working Directory**: `/project/tma1/ndnsf-di`
- **Duration**: 32 seconds
- **Exit Code**: 1
- **Slurm Job**: 181072
- **Node**: itiger07

### Output Files

| File | Size |
|---|---:|
| `hostname.txt` | 9 bytes |
| `nvidia-smi.txt` | 182 bytes |
| `result-terminal.json` | 158 bytes |
| `run.log` | 16,453 bytes |
| `source-checksums.log` | 377 bytes |

All files remain under:

`/project/tma1/ndnsf-di/evidence/spec166/standalone/.spec166-standalone-qwen3-003.partial`

### Output Summary

The allocation exposed the requested NVIDIA RTX 5000 Ada GPU. The candidate
interpreter imported both NDNSF-DI and the Qwen pipeline, and the source
checksum gate passed. ONNX Runtime then rejected the first stage during session
initialization because graph nodes were assigned to the default CPU execution
provider while `session.disable_cpu_ep_fallback=1` was active. No inference
request, generated token, or latency row was produced.

### Anomalies Detected

- Terminal failure: ONNX Runtime CPU-EP assignment rejected by the strict
  fail-closed session policy.
- Runtime warning: one `Memcpy` node was inserted for
  `CUDAExecutionProvider`.
- Non-terminal noise: ONNX Runtime thread-affinity calls targeted CPUs outside
  the Slurm task's allowed CPU set.
- No stall, timeout, automatic retry, or three-node submission occurred.
