# T025 Tiger control: current candidate

**Date:** 2026-08-29  
**Candidate:** `spec175-runtime-fe285147`  
**Source revision:** `15f823a4bb5a158be86df81074aae48d2a0244d7`  
**Runtime SIF:** `/project/tma1/ndnsf-di/releases/spec175-runtime-fe285147/runtime.sif`  
**SIF SHA-256:** `sha256:6d905dbb44a65b02dcd091fb1c9a89b81ce7bb86551acf16db2a620c8602e7d7`

## Submission and result

The current candidate passed the remote candidate/checklist binding before
Slurm submission. The remote submission used the repository-owned
`packaging/ndnsf-di-container/jobs/spec175/submit.sh control` path; no build,
overlay, or replacement SIF was performed on Tiger.

| Item | Evidence |
|---|---|
| Tiger job | `206901`, node `itiger01`, partition `bigTiger`, `--gres=gpu:0` |
| Slurm result | `COMPLETED`, elapsed `00:00:51`, exit `0:0` |
| Terminal result | `/project/tma1/ndnsf-di/submits/spec175-runtime-fe285147/spec175-control-206901/spec175-gate-terminal.json` |
| Terminal SHA-256 | `45c0bc8c6b9d6a563d189d23e386299a93e0721c620a386d42182ba6d89312bf` |
| Target Apptainer | `1.5.3-1.el9` on a `bigTiger` allocation |

The terminal record is:

```json
{"schemaVersion":"spec175-sif-control-v1","gate":"control","exitCode":0,"status":"PASS"}
```

## Control-path observations

The four-provider HELLO control completed with four validated ACKs and one
final response:

```text
NDNSF_DI_ACK_CLOSED count=4 successful=4 providers=/example/hello/provider/A,/example/hello/provider/C,/example/hello/provider/B,/example/hello/provider/D
NDNSF_DI_CONTROL_LIFECYCLE_PASS acks=4 response=HELLO_FROM_A
```

The full shared logs remain at
`/project/tma1/ndnsf-di/submits/spec175-runtime-fe285147/spec175-control-206901/`.
Their recorded hashes are retained with the remote submission manifest.
This is a deployment/control result only: it does not claim Qwen3.6-27B CUDA
execution, conversation residency, or performance.
