# Job 182782: bounded resource-repaired large-model failure

Job `182782` was the one and only FR-019 resource-repaired replacement for
v93 Job 182780. Its immutable campaign identity is
`spec168-campaign-v3-6490395b0b976a05175f`, with source bundle
`sha256:e44082c8e2cf76d58463d2fd889bcc2ca442765955efd5b8e54fe13e8886b9e6`,
source identity
`sha256:ad8404ab06826c7e5ab2e85723989c5d6783c20c829c65645236fbb4c1e3808a`,
the exact SIF
`sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`,
and large stage manifest
`sha256:16de41986564c436a33cd6a292f17f4bd939d0a893c441441022e3b389737f6f`.

The new immutable resource profile was `bigTiger`, three nodes, one RTX 5000
per node, four CPUs per task, `96 GiB/node`, and a two-hour limit. Gate C v94
passed before submission. The remote failure directory is:

`/project/tma1/ndnsf-di/evidence/spec168/tiger-large-single/`
`spec168-campaign-v3-6490395b0b976a05175f-FAILED-job-182782`

The request passed source/SIF/stage/artifact checks, secured ACK closure,
graph/placement, Selection commit, and all three role-specific Repository
fetches. The three verified shard sizes were 18,530,194,174 bytes,
15,987,396,002 bytes, and 19,274,718,182 bytes. All three Providers then
reported:

```text
LLM_PIPELINE_QWEN_RUNTIME_READY ... device=cuda:0
loadCompleted=true warmupCompleted=true cpuFallbackCount=0 deviceClass=CUDA
```

The runtime did return one authenticated large Response and resolved an 8,468-
byte large-response reference, but the deterministic reference check rejected
the generated sequence: 64 generated tokens, `wireRequestCount=1`,
`tokenRequestCount=0`, `status=FAILED`, `stopReason=TOKEN_MISMATCH`, and
`exactReferenceMatch=false` (`expected=64 actual=64`). Total response time was
2,042,080.9 ms and TTFT was 2,008,431.8 ms. Slurm nevertheless recorded
`182782.0 FAILED` (exit `1:0`) because the analyzer requires an exact reference
match; the remote directory contains `terminal-state.txt=FAIL`,
`rank-failed-0.txt`, and `outer-rank-failed-0.txt`. The immutable raw evidence
hashes are `generation-raw.jsonl=sha256:4504acd6e616d6e8673ad72652d9fdcd5a99ad968b8caf8f5cf10a430ab2a882`
and `user.log=sha256:5c23f1042fd41bb5e3e60877b9b44de3a1388e1687f58ef1b9682a6fbd6beb2f`.
The first token divergence is at index 40 (reference token `103906`, staged
token `96902`); the first 40 generated tokens match. No top-k/tie evidence was
recorded, so this must not be relabeled as a numerically equivalent divergence.

No kernel memory-cgroup OOM was observed for this job. Because the campaign
contract allows only this one resource-repaired identity, this deterministic
response mismatch closes the large-model attempt without another retry. T015
remains open: the transport/security path is evidenced, but the required exact
deterministic three-stage acceptance is not.
